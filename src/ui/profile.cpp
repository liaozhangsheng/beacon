#include <beacon/ui/profile.hpp>
#include <beacon/core/model.hpp>
#include <beacon/io/file.hpp>
#include <beacon/http/client.hpp>
#include <beacon/minecraft/adapter.hpp>
#include "../core/json.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string_view>

namespace beacon {
namespace {

constexpr std::size_t max_skin_bytes = 4U * 1024U * 1024U;
constexpr std::string_view skin_host = "textures.minecraft.net/";
// Players change skins and names; an older cached skin still shows, but is downloaded again.
constexpr auto skin_cache_lifetime = std::chrono::hours(24);
// A failed download is retried after this, doubling up to the maximum while it keeps failing.
constexpr auto retry_initial_delay = std::chrono::seconds(30);
constexpr auto retry_max_delay = std::chrono::minutes(15);

std::filesystem::path skin_cache_directory(const std::filesystem::path& install_root) {
    return install_root / "cache/skins";
}

std::string cache_component(std::string value) {
    for (auto& character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (!std::isalnum(byte) && character != '-' && character != '_') {
            character = '_';
        }
    }
    return value;
}

struct CachedSkin {
    PlayerCard card;
    bool fresh = false;
};

std::optional<CachedSkin> read_cached_skin(const std::string& uuid, const std::filesystem::path& install_root) {
    const auto prefix = cache_component(uuid) + "_";
    std::error_code ec;
    std::filesystem::directory_iterator files(skin_cache_directory(install_root), ec);
    if (ec) {
        return std::nullopt;
    }
    for (; files != std::filesystem::directory_iterator{};) {
        const auto path = files->path();
        files.increment(ec);
        if (ec) {
            return std::nullopt;
        }
        std::error_code file_ec;
        if (!std::filesystem::is_regular_file(path, file_ec) || file_ec || path.extension() != ".png") {
            continue;
        }
        const auto filename = path_to_utf8(path.filename());
        if (!filename.starts_with(prefix)) {
            continue;
        }
        auto skin = read_bounded_file(path, "cached skin", max_skin_bytes);
        if (skin && !skin->empty()) {
            const auto written = std::filesystem::last_write_time(path, file_ec);
            const bool fresh =
                !file_ec && std::filesystem::file_time_type::clock::now() - written < skin_cache_lifetime;
            return CachedSkin{
                .card = PlayerCard{.name = path_to_utf8(path.stem()).substr(prefix.size()), .skin = std::move(*skin)},
                .fresh = fresh};
        }
    }
    return std::nullopt;
}

// Replaces every cached skin for uuid, so a renamed player leaves no entry under the old name.
void write_cached_skin(const std::string& uuid, const PlayerCard& card, const std::filesystem::path& install_root) {
    std::error_code ec;
    const auto directory = skin_cache_directory(install_root);
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        return;
    }
    const auto prefix = cache_component(uuid) + "_";
    const auto path = directory / (prefix + cache_component(card.name) + ".png");
    if (!write_file_atomically(path, card.skin)) {
        return;
    }
    std::filesystem::directory_iterator files(directory, ec);
    for (; !ec && files != std::filesystem::directory_iterator{}; files.increment(ec)) {
        const auto& old = files->path();
        if (old != path && path_to_utf8(old.filename()).starts_with(prefix) && old.extension() == ".png") {
            std::error_code remove_ec;
            std::filesystem::remove(old, remove_ec);
        }
    }
}

std::optional<std::string> download_profile_asset(const std::string& url, const std::size_t max_bytes,
                                                  const std::stop_token stop) {
    auto body = http::get_text(
        url, http::Limits{.max_bytes = max_bytes, .timeout_seconds = 20, .connect_timeout_seconds = 10}, stop);
    if (!body)
        return std::nullopt;
    return std::move(*body);
}

// Mojang's profile endpoint takes the undashed form; anything else is not an online-mode UUID. Offline-mode
// servers derive version 3 UUIDs from the name, which Mojang never knows.
std::optional<std::string> undashed_uuid(std::string_view uuid) {
    std::string result;
    for (const auto character : uuid) {
        if (character == '-')
            continue;
        if (!std::isxdigit(static_cast<unsigned char>(character)))
            return std::nullopt;
        result.push_back(character);
    }
    if (result.size() != 32 || result[12] == '3')
        return std::nullopt;
    return result;
}

std::optional<std::string> decode_base64(std::string_view text) {
    std::string result;
    std::uint32_t buffer = 0;
    int bits = 0;
    for (const auto character : text) {
        if (character == '=')
            break;
        int value = -1;
        if (character >= 'A' && character <= 'Z')
            value = character - 'A';
        else if (character >= 'a' && character <= 'z')
            value = character - 'a' + 26;
        else if (character >= '0' && character <= '9')
            value = character - '0' + 52;
        else if (character == '+')
            value = 62;
        else if (character == '/')
            value = 63;
        if (value < 0)
            return std::nullopt;
        buffer = (buffer << 6U) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            result.push_back(static_cast<char>((buffer >> static_cast<unsigned>(bits)) & 0xFFU));
        }
    }
    return result;
}

// Only Mojang's texture host is trusted, upgraded to HTTPS because the profile still lists http:// URLs.
std::optional<std::string> skin_url(const Json::Value& textures) {
    const auto& url = textures["textures"]["SKIN"]["url"];
    if (!url.isString() || url.asString().size() > max_string_bytes)
        return std::nullopt;
    std::string_view value = url.asCString();
    for (const std::string_view scheme : {"http://", "https://"}) {
        if (value.starts_with(scheme)) {
            value.remove_prefix(scheme.size());
            break;
        }
    }
    if (!value.starts_with(skin_host))
        return std::nullopt;
    return "https://" + std::string(value);
}

// The player's card from Mojang, or nullopt when a request or its answer fails.
std::optional<PlayerCard> download_player_card(const std::string& uuid, const std::filesystem::path& install_root,
                                               const std::stop_token stop) {
    const auto id = undashed_uuid(uuid);
    if (!id) {
        return PlayerCard{};
    }
    const auto profile = download_profile_asset("https://sessionserver.mojang.com/session/minecraft/profile/" + *id,
                                                max_json_bytes, stop);
    if (!profile) {
        return std::nullopt;
    }
    auto root = parse_json(*profile, "player profile", "invalid player profile JSON");
    if (!root || !root->isObject() || !(*root)["name"].isString() || !(*root)["properties"].isArray()) {
        return std::nullopt;
    }
    const auto name = (*root)["name"].asString();
    if (name.empty() || name.size() > max_string_bytes || !valid_utf8(name)) {
        return std::nullopt;
    }
    std::optional<std::string> url;
    for (const auto& property : (*root)["properties"]) {
        if (!property.isObject() || property["name"] != "textures" || !property["value"].isString())
            continue;
        const auto decoded = decode_base64(property["value"].asString());
        if (!decoded)
            break;
        auto textures = parse_json(*decoded, "player textures", "invalid player textures JSON");
        if (textures && textures->isObject())
            url = skin_url(*textures);
        break;
    }
    // Players on the default skin have no SKIN entry; the bundled avatar stands in for them.
    if (!url) {
        return PlayerCard{.name = name};
    }
    auto skin = download_profile_asset(*url, max_skin_bytes, stop);
    if (!skin || skin->empty() || stop.stop_requested()) {
        return std::nullopt;
    }
    PlayerCard card{.name = name, .skin = std::move(*skin)};
    write_cached_skin(uuid, card, install_root);
    return card;
}

}  // namespace

std::optional<PlayerCard> fetch_player_card(std::string uuid, const std::filesystem::path& install_root,
                                            const std::stop_token stop) {
    try {
        if (!valid_player_id(uuid)) {
            return PlayerCard{};
        }
        if (stop.stop_requested()) {
            return std::nullopt;
        }
        auto cached = read_cached_skin(uuid, install_root);
        if (cached && cached->fresh) {
            return std::move(cached->card);
        }
        auto card = download_player_card(uuid, install_root, stop);
        if (!card && cached) {
            return std::move(cached->card);
        }
        return card;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<PlayerCard> cached_player_card(const std::string& uuid, const std::filesystem::path& install_root) {
    try {
        auto cached = valid_player_id(uuid) ? read_cached_skin(uuid, install_root) : std::nullopt;
        return cached ? std::optional(std::move(cached->card)) : std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

ProfileLoader::ProfileLoader(std::filesystem::path install_root)
    : install_root_(std::move(install_root)), worker_([this](std::stop_token stop) {
          run(stop);
      }) {}

ProfileLoader::~ProfileLoader() {
    worker_.request_stop();
    {
        std::scoped_lock lock(mutex_);
        request_stop_.request_stop();
    }
    wake_.notify_all();
}

std::shared_ptr<const PlayerCard> ProfileLoader::update(const std::string_view uuid) {
    std::scoped_lock lock(mutex_);
    if (requested_uuid_ != uuid) {
        request_stop_.request_stop();
        request_stop_ = std::stop_source{};
        requested_uuid_ = std::string(uuid);
        current_ = std::make_shared<const PlayerCard>();
        result_.reset();
        pending_uuid_ = requested_uuid_;
        wake_.notify_one();
    }
    if (result_ && result_->first == requested_uuid_) {
        current_ = std::move(result_->second);
        result_.reset();
    }
    return current_;
}

void ProfileLoader::publish(std::string uuid, PlayerCard card, const std::stop_token& request_stop) {
    auto shared = std::make_shared<const PlayerCard>(std::move(card));
    std::scoped_lock lock(mutex_);
    if (!request_stop.stop_requested() && uuid == requested_uuid_)
        result_ = std::pair{std::move(uuid), std::move(shared)};
}

void ProfileLoader::run(const std::stop_token stop) {
    while (!stop.stop_requested()) {
        std::string uuid;
        std::stop_token request_stop;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, stop, [this] {
                return pending_uuid_.has_value();
            });
            if (stop.stop_requested())
                return;
            uuid = std::move(*pending_uuid_);
            pending_uuid_.reset();
            request_stop = request_stop_.get_token();
        }
        if (uuid.empty())
            continue;
        // An older cached skin shows while the download runs or keeps failing.
        try {
            auto cached = valid_player_id(uuid) ? read_cached_skin(uuid, install_root_) : std::nullopt;
            if (cached && cached->fresh) {
                publish(std::move(uuid), std::move(cached->card), request_stop);
                continue;
            }
            if (cached)
                publish(uuid, std::move(cached->card), request_stop);
        } catch (...) {
        }
        // Offline at launch or a Mojang hiccup: try again later instead of keeping Steve until a restart.
        for (auto delay = std::chrono::steady_clock::duration(retry_initial_delay);;) {
            std::optional<PlayerCard> card;
            try {
                card = valid_player_id(uuid) ? download_player_card(uuid, install_root_, request_stop) : PlayerCard{};
            } catch (...) {
            }
            if (card) {
                publish(std::move(uuid), std::move(*card), request_stop);
                break;
            }
            std::unique_lock lock(mutex_);
            if (wake_.wait_for(lock, stop, delay,
                               [this, &request_stop] {
                                   return pending_uuid_.has_value() || request_stop.stop_requested();
                               }) ||
                stop.stop_requested())
                break;
            delay = std::min<std::chrono::steady_clock::duration>(delay * 2, retry_max_delay);
        }
    }
}

}  // namespace beacon
