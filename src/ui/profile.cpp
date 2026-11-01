#include <beacon/ui/profile.hpp>
#include <beacon/core/model.hpp>
#include <beacon/io/file.hpp>
#include <beacon/http/client.hpp>
#include <beacon/minecraft/adapter.hpp>
#include "../core/json.hpp"

#include <algorithm>
#include <cctype>
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

std::filesystem::path skin_cache_directory(const std::filesystem::path& data_root) {
    return data_root / "cache/skins";
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

std::optional<PlayerCard> read_cached_skin(const std::string& uuid, const std::filesystem::path& data_root) {
    const auto prefix = cache_component(uuid) + "_";
    std::error_code ec;
    std::filesystem::directory_iterator files(skin_cache_directory(data_root), ec);
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
            return PlayerCard{.name = path_to_utf8(path.stem()).substr(prefix.size()), .skin = std::move(*skin)};
        }
    }
    return std::nullopt;
}

void write_cached_skin(const std::string& uuid, const PlayerCard& card, const std::filesystem::path& data_root) {
    std::error_code ec;
    const auto directory = skin_cache_directory(data_root);
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        return;
    }
    const auto path = directory / (cache_component(uuid) + "_" + cache_component(card.name) + ".png");
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (output) {
        output.write(card.skin.data(), static_cast<std::streamsize>(card.skin.size()));
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

// Mojang's profile endpoint takes the undashed form; anything else is not an online-mode UUID.
std::optional<std::string> undashed_uuid(std::string_view uuid) {
    std::string result;
    for (const auto character : uuid) {
        if (character == '-')
            continue;
        if (!std::isxdigit(static_cast<unsigned char>(character)))
            return std::nullopt;
        result.push_back(character);
    }
    if (result.size() != 32)
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

}  // namespace

PlayerCard fetch_player_card(std::string uuid, const std::filesystem::path& data_root, const std::stop_token stop) {
    try {
        if (stop.stop_requested() || !valid_player_id(uuid)) {
            return {};
        }
        // Earlier builds cached third-party head renders; skins replaced them.
        std::error_code ec;
        std::filesystem::remove_all(data_root / "cache/avatars", ec);
        if (auto cached = read_cached_skin(uuid, data_root)) {
            return std::move(*cached);
        }
        const auto id = undashed_uuid(uuid);
        if (!id) {
            return {};
        }
        const auto profile = download_profile_asset("https://sessionserver.mojang.com/session/minecraft/profile/" + *id,
                                                    max_json_bytes, stop);
        if (!profile) {
            return {};
        }
        auto root = parse_json(*profile, "player profile", "invalid player profile JSON");
        if (!root || !root->isObject() || !(*root)["name"].isString() || !(*root)["properties"].isArray()) {
            return {};
        }
        const auto name = (*root)["name"].asString();
        if (name.empty() || name.size() > max_string_bytes || !valid_utf8(name)) {
            return {};
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
        if (!skin || stop.stop_requested()) {
            return {};
        }
        PlayerCard card{.name = name, .skin = std::move(*skin)};
        write_cached_skin(uuid, card, data_root);
        return card;
    } catch (...) {
        return {};
    }
}

ProfileLoader::ProfileLoader(std::filesystem::path data_root)
    : data_root_(std::move(data_root)), worker_([this](std::stop_token stop) {
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
        auto card = std::make_shared<const PlayerCard>(fetch_player_card(uuid, data_root_, request_stop));
        std::scoped_lock lock(mutex_);
        if (!request_stop.stop_requested() && uuid == requested_uuid_)
            result_ = std::pair{std::move(uuid), std::move(card)};
    }
}

}  // namespace beacon
