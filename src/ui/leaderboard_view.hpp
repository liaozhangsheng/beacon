#pragma once

#include <beacon/ui/frame_schedule.hpp>
#include <beacon/ui/leaderboard.hpp>
#include <beacon/ui/widgets.hpp>

#include <imgui.h>

#include <string>
#include <string_view>
#include <unordered_map>

namespace beacon {

// One board of the community leaderboard in a main-window layout group: each run a toast with the player's head,
// rank badge, IGT and date.
class LeaderboardView {
public:
    // Draws the board `source` names into [min, max], starting its download the first time. Podium runs glow with
    // `glow`, a neutral additive glow that each placing tints to its metal, when there is one.
    void draw(ImVec2 min, ImVec2 max, const LeaderboardSource& source, LeaderboardLoader& loader, SDL_Texture* glow,
              UiAssets& assets, SDL_Renderer* renderer, std::string_view language, FrameSchedule& schedule);

private:
    std::unordered_map<std::string, SdlTexturePtr> heads_;
};

}  // namespace beacon
