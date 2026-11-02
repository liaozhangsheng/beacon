#pragma once

#include <beacon/ui/display.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace beacon {

struct LeaderboardRun {
    int rank = 0;
    std::string runner;
    std::string igt;
    // YYYY-MM-DD, or empty when the sheet's date cannot be read.
    std::string date;
    // The run's video or speedrun.com page; empty unless the sheet gives an http(s) address.
    std::string link;
};

struct LeaderboardBoard {
    std::string title;
    std::vector<LeaderboardRun> runs;
};

using LeaderboardBoards = std::vector<LeaderboardBoard>;

// A tab places boards side by side: a title row, then a header row where each board's columns start at "#".
// A board needs Runner and IGT columns; Date, Link and Status are optional, and rejected runs are left out.
LeaderboardBoards parse_leaderboard(std::string_view csv);

enum class LeaderboardStatus { Idle, Loading, Updating, Ready, Offline, Failed };

// Players' UUIDs by name, cached under install_root and shared by every loader.
class LeaderboardPlayers;

// Downloads one board's sheet tab and its players' skins off the UI thread. The tab and each player's UUID are
// cached under install_root, and skins share the tracker's cache, so the board shows at once and offline later.
// Nothing is fetched until start().
class LeaderboardLoader {
public:
    struct Update {
        std::shared_ptr<const LeaderboardBoards> boards;
        LeaderboardStatus status = LeaderboardStatus::Idle;
        // When the boards shown were downloaded, as local "YYYY-MM-DD HH:MM"; empty before any arrive.
        std::string downloaded;
        // Skins that arrived since the last take(), by runner; an empty PNG means the player has no skin.
        std::vector<std::pair<std::string, std::string>> skins;
    };

    LeaderboardLoader(std::filesystem::path install_root, std::shared_ptr<LeaderboardPlayers> players,
                      LeaderboardSource source);
    LeaderboardLoader(const LeaderboardLoader&) = delete;
    LeaderboardLoader& operator=(const LeaderboardLoader&) = delete;

    // Starts the download once; later calls do nothing.
    void start();
    // Downloads the tab again and retries missing heads, unless the tab is already downloading.
    void refresh();
    Update take();

private:
    void run(std::stop_token stop);
    // Starts run() in place of the current worker.
    void restart(LeaderboardStatus status);
    void publish(std::shared_ptr<const LeaderboardBoards> boards, LeaderboardStatus status, std::string downloaded);

    std::filesystem::path install_root_;
    std::shared_ptr<LeaderboardPlayers> players_;
    LeaderboardSource source_;
    std::mutex mutex_;
    std::shared_ptr<const LeaderboardBoards> boards_ = std::make_shared<const LeaderboardBoards>();
    LeaderboardStatus status_ = LeaderboardStatus::Idle;
    std::string downloaded_;
    std::vector<std::pair<std::string, std::string>> skins_;
    // Last, so it stops and joins before the state it writes is destroyed.
    std::jthread worker_;
};

// A loader for every board the layouts have shown, kept across template switches; UI thread only.
class LeaderboardLoaders {
public:
    explicit LeaderboardLoaders(std::filesystem::path install_root);
    ~LeaderboardLoaders();
    LeaderboardLoaders(const LeaderboardLoaders&) = delete;
    LeaderboardLoaders& operator=(const LeaderboardLoaders&) = delete;

    LeaderboardLoader& get(const LeaderboardSource& source);

private:
    std::filesystem::path install_root_;
    std::shared_ptr<LeaderboardPlayers> players_;
    std::map<std::tuple<std::string, std::uint64_t, std::string>, std::unique_ptr<LeaderboardLoader>> loaders_;
};

}  // namespace beacon
