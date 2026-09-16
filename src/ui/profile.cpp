#include <beacon/ui/profile.hpp>
#include <beacon/core/model.hpp>
#include <beacon/io/file.hpp>
#include <beacon/minecraft/adapter.hpp>
#include "../core/json.hpp"

#include <ylt/coro_http/coro_http_client.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string_view>

namespace beacon {
namespace {

constexpr std::size_t max_avatar_bytes = 4U * 1024U * 1024U;

std::filesystem::path avatar_cache_directory(const std::filesystem::path& data_root) {
    return data_root / "cache/avatars";
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

std::optional<PlayerCard> read_cached_avatar(const std::string& uuid, const std::filesystem::path& data_root) {
    const auto prefix = cache_component(uuid) + "_";
    std::error_code ec;
    std::filesystem::directory_iterator files(avatar_cache_directory(data_root), ec);
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
        const auto filename = path.filename().string();
        if (!filename.starts_with(prefix)) {
            continue;
        }
        auto avatar = read_bounded_file(path, "cached avatar", max_avatar_bytes);
        if (avatar && !avatar->empty()) {
            return PlayerCard{.name = path.stem().string().substr(prefix.size()), .avatar = std::move(*avatar)};
        }
    }
    return std::nullopt;
}

void write_cached_avatar(const std::string& uuid, const PlayerCard& card, const std::filesystem::path& data_root) {
    std::error_code ec;
    const auto directory = avatar_cache_directory(data_root);
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        return;
    }
    const auto path = directory / (cache_component(uuid) + "_" + cache_component(card.name) + ".png");
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (output) {
        output.write(card.avatar.data(), static_cast<std::streamsize>(card.avatar.size()));
    }
}

}  // namespace

std::optional<std::string> download_profile_asset(const std::string& url, const std::size_t max_bytes,
                                                  const std::stop_token stop) {
    // URI parsing and TLS verification must use the same, unambiguous hostname.
    if (url.size() > max_string_bytes || !std::all_of(url.begin(), url.end(), [](unsigned char c) {
            return c > 32 && c < 127;
        })) {
        return std::nullopt;
    }
    coro_http::uri_t uri;
    if (!uri.parse_from(url.c_str()) || uri.schema != "https" || uri.host.empty() || !uri.uinfo.empty()) {
        return std::nullopt;
    }
    if (stop.stop_requested())
        return std::nullopt;
    // Run I/O on the existing profile worker. Do not start the global hardware-sized pool.
    asio::io_context context;
    asio::ip::tcp::resolver resolver(context);
    coro_http::coro_http_client client(context.get_executor());
    if (!client.init_ssl(asio::ssl::verify_peer, "", std::string(uri.host))) {
        return std::nullopt;
    }
    client.set_max_http_body_size(static_cast<std::int64_t>(max_bytes));
    client.add_header("User-Agent", "Beacon/0.1");
    client.set_conn_timeout(std::chrono::seconds(10));
    client.set_req_timeout(std::chrono::seconds(10));
    bool cancelled = false;
    std::optional<std::string> result;
    asio::steady_timer deadline(context, std::chrono::seconds(20));
    const auto cancel = [&] {
        cancelled = true;
        resolver.cancel();
        client.close();
    };
    deadline.async_wait([&](const std::error_code& error) {
        if (!error)
            cancel();
    });
    {
        std::stop_callback on_stop(stop, [&] {
            asio::post(context, cancel);
        });
        const auto request = [&]() -> async_simple::coro::Lazy<void> {
            // Own the resolver so cancellation also covers DNS, before a socket exists.
            auto [error, resolved] =
                co_await coro_io::async_io<std::pair<std::error_code, asio::ip::tcp::resolver::results_type>>(
                    [&](auto&& callback) {
                        resolver.async_resolve(uri.get_host(), uri.get_port(), std::move(callback));
                    },
                    resolver);
            if (error || cancelled || stop.stop_requested())
                co_return;
            std::vector<asio::ip::tcp::endpoint> endpoints;
            for (const auto& entry : resolved)
                endpoints.push_back(entry.endpoint());
            const auto connected = co_await client.connect(url, &endpoints);
            if (connected.net_err || cancelled || stop.stop_requested())
                co_return;
            const auto response = co_await client.async_get(url);
            if (!cancelled && !stop.stop_requested() && !response.net_err && response.status >= 200 &&
                response.status < 300 && response.resp_body.size() <= max_bytes)
                result = std::string(response.resp_body);
        };
        request().start([&](auto&&) {
            deadline.cancel();
        });
        context.run();
    }
    // Drain a stop callback posted just as the request completed, before destroying the client.
    context.restart();
    context.poll();
    return stop.stop_requested() ? std::nullopt : result;
}

PlayerCard fetch_player_card(std::string uuid, const std::filesystem::path& data_root, const std::stop_token stop) {
    try {
        if (stop.stop_requested() || !valid_player_id(uuid)) {
            return {};
        }
        if (auto cached = read_cached_avatar(uuid, data_root)) {
            return std::move(*cached);
        }
        const auto profile =
            download_profile_asset("https://playerdb.co/api/player/minecraft/" + uuid, max_json_bytes, stop);
        if (!profile) {
            return {};
        }
        auto root = parse_json(*profile, "player profile", "invalid player profile JSON");
        if (!root) {
            return {};
        }
        const auto& player = (*root)["data"]["player"];
        if (!player.isObject() || !player["username"].isString() || !player["avatar"].isString()) {
            return {};
        }
        const auto name = player["username"].asString();
        const auto avatar_url = player["avatar"].asString();
        if (name.empty() || name.size() > max_string_bytes || !valid_utf8(name) ||
            !avatar_url.starts_with("https://") || avatar_url.size() > max_string_bytes) {
            return {};
        }
        auto avatar = download_profile_asset(avatar_url, max_avatar_bytes, stop);
        if (!avatar || stop.stop_requested()) {
            return {};
        }
        PlayerCard card{.name = name, .avatar = std::move(*avatar)};
        write_cached_avatar(uuid, card, data_root);
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
