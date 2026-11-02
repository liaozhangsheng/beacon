#pragma once

#include <beacon/ui/frame_schedule.hpp>
#include <beacon/ui/widgets.hpp>

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <unordered_map>
#include <vector>

namespace beacon {

// Minecraft's enchantment glint: a soft purple streak texture scrolls across the item and is added onto its
// opaque pixels. The streaks are procedural so no Mojang texture has to ship.
class EnchantmentGlint {
public:
    // Minecraft's glint texture is 128 texels wide and its streaks repeat every 64.
    static constexpr std::size_t streak_period = 64;
    using Streaks = std::array<float, streak_period>;
    // Overlays only change on this step: each change re-uploads the texture, and the drift needs no more.
    static constexpr std::uint64_t frame_ms = soft_animation_ms;

    explicit EnchantmentGlint(SDL_Renderer* renderer);

    // An additive overlay shaped like the icon, or null when the icon cannot be read.
    // Every overlay is refreshed at most once per frame_ms step, so shared icons cost one update per frame.
    SDL_Texture* overlay(const std::filesystem::path& icon, std::uint64_t now_ms);
    void clear() {
        overlays_.clear();
    }

private:
    struct Overlay {
        SdlTexturePtr texture;
        int size = 0;
        // Overlay alpha per pixel: 255 where the icon is opaque enough to carry the glint, else 0.
        std::vector<std::uint8_t> mask;
        // Where the icon sits in the glint texture, like an item's place in Minecraft's atlas.
        float offset_u = 0.0F;
        float offset_v = 0.0F;
        std::uint64_t updated_ms = UINT64_MAX;
    };

    // Leaves the texture null when the icon cannot be read.
    void load(Overlay& overlay, const std::filesystem::path& icon) const;
    void update(Overlay& overlay, std::uint64_t now_ms) const;

    SDL_Renderer* renderer_;
    // The texture is two families of streaks: vertical ones and ones running along x + 2y. Each holds brightness
    // offsets around the texture's mean.
    Streaks vertical_streaks_;
    Streaks diagonal_streaks_;
    std::unordered_map<std::filesystem::path, Overlay> overlays_;
};

}  // namespace beacon
