#pragma once

#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace beacon {

struct PlayerCard {
    std::string name = "Steve";
    std::string avatar;
};

std::optional<std::string> download_profile_asset(const std::string& url, std::size_t max_bytes,
                                                  std::stop_token stop = {});

PlayerCard fetch_player_card(std::string uuid, const std::filesystem::path& data_root, std::stop_token stop = {});

// Owns the one-at-a-time UI profile refresh and keeps network work off the frame loop.
class ProfileLoader {
public:
    explicit ProfileLoader(std::filesystem::path data_root);
    ~ProfileLoader();
    ProfileLoader(const ProfileLoader&) = delete;
    ProfileLoader& operator=(const ProfileLoader&) = delete;
    std::shared_ptr<const PlayerCard> update(std::string_view uuid);

private:
    void run(std::stop_token stop);
    std::filesystem::path data_root_;
    std::mutex mutex_;
    std::condition_variable_any wake_;
    std::string requested_uuid_;
    std::optional<std::string> pending_uuid_;
    std::stop_source request_stop_;
    std::shared_ptr<const PlayerCard> current_ = std::make_shared<const PlayerCard>();
    std::optional<std::pair<std::string, std::shared_ptr<const PlayerCard>>> result_;
    std::jthread worker_;
};

}  // namespace beacon
