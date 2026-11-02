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
    // Raw skin PNG from Mojang's texture host; the renderer crops the head from it.
    std::string skin;
};

// A cached skin younger than a day, or Mojang's; an older cached skin when Mojang cannot be reached. nullopt means
// the request failed and is worth retrying; offline-mode players get the default card.
std::optional<PlayerCard> fetch_player_card(std::string uuid, const std::filesystem::path& install_root,
                                            std::stop_token stop = {});
// The skin fetch_player_card cached for uuid, of any age, without any request.
std::optional<PlayerCard> cached_player_card(const std::string& uuid, const std::filesystem::path& install_root);

// Owns the one-at-a-time UI profile refresh and keeps network work off the frame loop. A failed download is retried
// with backoff until the player changes.
class ProfileLoader {
public:
    explicit ProfileLoader(std::filesystem::path install_root);
    ~ProfileLoader();
    ProfileLoader(const ProfileLoader&) = delete;
    ProfileLoader& operator=(const ProfileLoader&) = delete;
    std::shared_ptr<const PlayerCard> update(std::string_view uuid);

private:
    void run(std::stop_token stop);
    void publish(std::string uuid, PlayerCard card, const std::stop_token& request_stop);
    std::filesystem::path install_root_;
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
