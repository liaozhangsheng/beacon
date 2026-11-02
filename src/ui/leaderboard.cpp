#include <beacon/ui/leaderboard.hpp>
#include <beacon/http/client.hpp>
#include <beacon/io/file.hpp>
#include <beacon/ui/profile.hpp>
#include "../core/json.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <format>
#include <map>
#include <mutex>
#include <optional>
#include <set>

namespace beacon {
namespace {

constexpr std::size_t max_sheet_bytes = 4U * 1024U * 1024U;
// Heads past this rank come only from the cache: each new player costs three Mojang requests, and long boards would
// run into its rate limit.
constexpr std::size_t max_online_heads = 50;

// Mojang usernames are 1-16 word characters; anything else never reaches a URL.
bool valid_username(const std::string_view name) {
    return !name.empty() && name.size() <= 16 && std::ranges::all_of(name, [](const char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    });
}

// A time as local "YYYY-MM-DD HH:MM".
std::string local_time(const std::chrono::system_clock::time_point time) {
    const std::time_t seconds = std::chrono::system_clock::to_time_t(time);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif
    char text[32] = {};
    std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M", &local);
    return text;
}

// Mojang answers with undashed UUIDs; saves, and so the skin cache, use the dashed form.
std::string dashed_uuid(const std::string& id) {
    if (id.size() != 32)
        return id;
    return std::format("{}-{}-{}-{}-{}", id.substr(0, 8), id.substr(8, 4), id.substr(12, 4), id.substr(16, 4),
                       id.substr(20));
}

}  // namespace

// With the UUID and the skin both cached, a head needs no request at all.
class LeaderboardPlayers {
public:
    explicit LeaderboardPlayers(std::filesystem::path path) : path_(std::move(path)) {
        const auto text = read_bounded_file(path_, "leaderboard players");
        const auto root = text ? parse_json(*text, "leaderboard players", "invalid leaderboard players JSON")
                               : ylt::expected<Json::Value, Error>{};
        if (!root || !root->isObject())
            return;
        for (const auto& name : root->getMemberNames()) {
            if ((*root)[name].isString())
                ids_[name] = (*root)[name].asString();
        }
    }

    std::optional<std::string> known(const std::string& name) const {
        std::scoped_lock lock(mutex_);
        const auto found = ids_.find(name);
        return found == ids_.end() ? std::nullopt : std::optional(found->second);
    }

    std::string get(const std::string& name, const std::stop_token stop) {
        if (auto id = known(name))
            return std::move(*id);
        if (!valid_username(name))
            return {};
        const auto body = http::get_text("https://api.mojang.com/users/profiles/minecraft/" + name,
                                         {.max_bytes = max_json_bytes, .timeout_seconds = 20}, stop);
        const auto profile = body ? parse_json(*body, "player lookup", "invalid player lookup JSON")
                                  : ylt::expected<Json::Value, Error>{};
        if (!profile || !profile->isObject() || !(*profile)["id"].isString())
            return {};
        auto id = dashed_uuid((*profile)["id"].asString());
        std::scoped_lock lock(mutex_);
        ids_[name] = id;
        Json::Value saved(Json::objectValue);
        for (const auto& [known, uuid] : ids_)
            saved[known] = uuid;
        (void)write_file_atomically(path_, Json::writeString(Json::StreamWriterBuilder{}, saved));
        return id;
    }

private:
    std::filesystem::path path_;
    mutable std::mutex mutex_;
    std::map<std::string, std::string> ids_;
};

LeaderboardLoader::LeaderboardLoader(std::filesystem::path install_root, std::shared_ptr<LeaderboardPlayers> players,
                                     LeaderboardSource source)
    : install_root_(std::move(install_root)), players_(std::move(players)), source_(std::move(source)) {}

void LeaderboardLoader::start() {
    std::scoped_lock lock(mutex_);
    if (!worker_.joinable())
        restart(LeaderboardStatus::Loading);
}

void LeaderboardLoader::refresh() {
    std::scoped_lock lock(mutex_);
    if (status_ != LeaderboardStatus::Loading && status_ != LeaderboardStatus::Updating)
        restart(boards_->empty() ? LeaderboardStatus::Loading : LeaderboardStatus::Updating);
}

void LeaderboardLoader::restart(const LeaderboardStatus status) {
    status_ = status;
    worker_ = std::jthread([this, previous = std::move(worker_)](const std::stop_token stop) mutable {
        // The worker this replaces may still be fetching heads; it stops at its next request. Joining it here
        // keeps the UI thread from waiting.
        previous = {};
        run(stop);
    });
}

LeaderboardLoader::Update LeaderboardLoader::take() {
    std::scoped_lock lock(mutex_);
    return {.boards = boards_, .status = status_, .downloaded = downloaded_, .skins = std::exchange(skins_, {})};
}

void LeaderboardLoader::publish(std::shared_ptr<const LeaderboardBoards> boards, const LeaderboardStatus status,
                                std::string downloaded) {
    std::scoped_lock lock(mutex_);
    boards_ = std::move(boards);
    status_ = status;
    downloaded_ = std::move(downloaded);
}

void LeaderboardLoader::run(const std::stop_token stop) {
    const auto cache = install_root_ / "cache/leaderboard";
    std::error_code ec;
    std::filesystem::create_directories(cache, ec);
    const auto cache_file = cache / std::format("{}-{}.csv", source_.sheet, source_.gid);
    std::set<std::string> loaded;
    // Heads already on disk show at once; the board's top runs are looked up online only once the sheet is settled,
    // so a slow or unknown player never holds back an update.
    const auto load_heads = [&](const LeaderboardBoards& boards, const bool online) {
        const auto board =
            source_.board.empty() ? boards.begin() : std::ranges::find(boards, source_.board, &LeaderboardBoard::title);
        if (board == boards.end())
            return;
        for (std::size_t rank = 0; rank < board->runs.size() && (!online || rank < max_online_heads); ++rank) {
            const auto& name = board->runs[rank].runner;
            if (stop.stop_requested())
                return;
            if (loaded.contains(name))
                continue;
            std::string skin;
            if (online) {
                // A failed lookup or download stays missing, so refresh() tries it again.
                const auto id = players_->get(name, stop);
                if (id.empty())
                    continue;
                auto card = fetch_player_card(id, install_root_, stop);
                if (!card)
                    continue;
                skin = std::move(card->skin);
                loaded.insert(name);
            } else {
                // A cached head shows at once; the online pass still replaces it once it is a day old.
                const auto id = players_->known(name);
                auto card = id ? cached_player_card(*id, install_root_) : std::nullopt;
                if (!card)
                    continue;
                skin = std::move(card->skin);
            }
            std::scoped_lock lock(mutex_);
            skins_.emplace_back(name, std::move(skin));
        }
    };

    std::shared_ptr<const LeaderboardBoards> cached;
    std::string cached_time;
    if (const auto text = read_bounded_file(cache_file, "cached leaderboard", max_sheet_bytes)) {
        if (auto boards = parse_leaderboard(*text); !boards.empty()) {
            cached = std::make_shared<const LeaderboardBoards>(std::move(boards));
            if (const auto written = std::filesystem::last_write_time(cache_file, ec); !ec) {
                cached_time = local_time(std::chrono::system_clock::now() +
                                         std::chrono::duration_cast<std::chrono::system_clock::duration>(
                                             written - std::filesystem::file_time_type::clock::now()));
            }
            publish(cached, LeaderboardStatus::Updating, cached_time);
            load_heads(*cached, false);
        }
    }
    const auto url =
        std::format("https://docs.google.com/spreadsheets/d/{}/export?format=csv&gid={}", source_.sheet, source_.gid);
    const auto csv = http::get_text(url, {.max_bytes = max_sheet_bytes, .timeout_seconds = 30}, stop);
    if (stop.stop_requested())
        return;
    auto live = csv ? parse_leaderboard(*csv) : LeaderboardBoards{};
    if (live.empty()) {
        if (!cached) {
            publish(std::make_shared<const LeaderboardBoards>(), LeaderboardStatus::Failed, {});
            return;
        }
        publish(cached, LeaderboardStatus::Offline, cached_time);
        load_heads(*cached, true);
        return;
    }
    (void)write_file_atomically(cache_file, *csv);
    const auto boards = std::make_shared<const LeaderboardBoards>(std::move(live));
    publish(boards, LeaderboardStatus::Ready, local_time(std::chrono::system_clock::now()));
    load_heads(*boards, false);
    load_heads(*boards, true);
}

LeaderboardLoaders::LeaderboardLoaders(std::filesystem::path install_root)
    : install_root_(std::move(install_root)),
      players_(std::make_shared<LeaderboardPlayers>(install_root_ / "cache/leaderboard/players.json")) {}

LeaderboardLoaders::~LeaderboardLoaders() = default;

LeaderboardLoader& LeaderboardLoaders::get(const LeaderboardSource& source) {
    auto& loader = loaders_[{source.sheet, source.gid, source.board}];
    if (!loader)
        loader = std::make_unique<LeaderboardLoader>(install_root_, players_, source);
    return *loader;
}

}  // namespace beacon
