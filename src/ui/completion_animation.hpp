#pragma once

#include <beacon/app/runtime.hpp>

#include <imgui.h>

#include <array>
#include <cstdint>
#include <cstddef>
#include <optional>
#include <random>
#include <unordered_map>
#include <vector>

namespace beacon {

class CompletionAnimator {
public:
    void reset(std::size_t result_count);
    void update(const PublishedState& state);
    [[nodiscard]] bool active(std::uint32_t node) const;
    [[nodiscard]] bool any_active() const {
        return !started_.empty();
    }
    void draw_cover(ImDrawList* draw, std::uint32_t node, ImVec2 top_left, ImVec2 size) const;

private:
    std::vector<std::uint8_t> previous_done_;
    std::unordered_map<std::uint32_t, float> started_;
    std::optional<std::uint64_t> run_epoch_;
};

class CompletionFireworks {
public:
    CompletionFireworks();
    void reset();
    void update(ImVec2 top_left, ImVec2 size, float delta_seconds);
    void draw(ImDrawList* draw, ImVec2 clip_min, ImVec2 clip_max) const;

private:
    struct Particle {
        ImVec2 origin;
        float scale = 1.0F;
        float x = 0.0F;
        float y = 0.0F;
        float z = 0.0F;
        float vx = 0.0F;
        float vy = 0.0F;
        float vz = 0.0F;
        ImVec4 color;
        int age = 0;
        int lifetime = 1;
        bool trail = true;
        bool twinkle = false;
        ImVec4 fade{1.0F, 244.0F / 255.0F, 156.0F / 255.0F, 1.0F};
    };

    static constexpr std::size_t max_particles = 384;
    using Slot = std::uint16_t;

    void explode(ImVec2 top_left, ImVec2 size);
    void reset_pool();
    bool append_particle(const Particle& particle);
    void release_particle(Slot slot);
    void release_oldest_particle();

    std::mt19937 random_{std::random_device{}()};
    std::array<Particle, max_particles> particles_{};
    std::array<Slot, max_particles> active_slots_{};
    std::array<Slot, max_particles> active_positions_{};
    std::array<Slot, max_particles> free_slots_{};
    std::array<std::uint64_t, max_particles> birth_order_{};
    std::size_t active_count_ = 0;
    std::size_t free_count_ = max_particles;
    std::uint64_t next_birth_order_ = 0;
    float next_explosion_ = 0.0F;
    float tick_accumulator_ = 0.0F;
};

}  // namespace beacon
