#include "completion_animation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <span>

namespace beacon {
namespace {

constexpr float animation_delay = 0.6F;
constexpr float animation_duration = 1.0F;
constexpr float animation_end_hold = 1.0F / 60.0F;
constexpr float tick_seconds = 0.05F;
constexpr float gravity = 0.004F;
constexpr float particle_gravity = gravity * 0.1F;
constexpr float pi = std::numbers::pi_v<float>;
constexpr float explosion_interval_min = 1.0F;
constexpr float explosion_interval_jitter = 1.0F;

float random_float(std::mt19937& random, const float low, const float high) {
    return std::uniform_real_distribution<float>(low, high)(random);
}

float random_gaussian(std::mt19937& random, const float deviation) {
    return std::normal_distribution<float>(0.0F, deviation)(random);
}

template <typename Integer> Integer random_integer(std::mt19937& random, const Integer low, const Integer high) {
    return std::uniform_int_distribution<Integer>(low, high)(random);
}

constexpr ImVec4 color(const int red, const int green, const int blue) {
    return {red / 255.0F, green / 255.0F, blue / 255.0F, 1.0F};
}

ImVec4 lerp_color(const ImVec4 a, const ImVec4 b, const float t) {
    return {a.x + ((b.x - a.x) * t), a.y + ((b.y - a.y) * t), a.z + ((b.z - a.z) * t), 1.0F};
}

}  // namespace

void CompletionAnimator::reset(const std::size_t result_count) {
    previous_done_.assign(result_count, 0);
    started_.clear();
    run_epoch_.reset();
}

void CompletionAnimator::update(const PublishedState& state) {
    if (state.data_status != DataStatus::Ready) {
        started_.clear();
        return;
    }
    if (!run_epoch_.has_value() || *run_epoch_ != state.snapshot.run_epoch) {
        run_epoch_ = state.snapshot.run_epoch;
        previous_done_.assign(state.snapshot.results.size(), 0);
        started_.clear();
    } else if (previous_done_.size() != state.snapshot.results.size()) {
        previous_done_.assign(state.snapshot.results.size(), 0);
        started_.clear();
    }
    const float now = static_cast<float>(ImGui::GetTime());
    for (std::size_t index = 0; index < previous_done_.size(); ++index) {
        const bool done = state.snapshot.results[index].done;
        const auto node = static_cast<std::uint32_t>(index);
        if (!previous_done_[index] && done) {
            started_[node] = now;
        } else if (!done) {
            started_.erase(node);
        } else if (const auto found = started_.find(node);
                   found != started_.end() &&
                   now - found->second >= animation_delay + animation_duration + animation_end_hold) {
            started_.erase(found);
        }
        previous_done_[index] = done;
    }
}

bool CompletionAnimator::active(const std::uint32_t node) const {
    return started_.contains(node);
}

void CompletionAnimator::draw_cover(ImDrawList* draw, const std::uint32_t node, const ImVec2 top_left,
                                    const ImVec2 size) const {
    const auto found = started_.find(node);
    if (found == started_.end()) {
        return;
    }
    const float elapsed = static_cast<float>(ImGui::GetTime()) - found->second - animation_delay;
    if (elapsed <= 0.0F) {
        return;
    }
    const float height = size.y * std::min(elapsed / animation_duration, 1.0F);
    draw->AddRectFilled({top_left.x, top_left.y + size.y - height}, {top_left.x + size.x, top_left.y + size.y},
                        ImGui::GetColorU32(ImGuiCol_WindowBg));
}

CompletionFireworks::CompletionFireworks() {
    reset_pool();
}

void CompletionFireworks::reset() {
    reset_pool();
    next_explosion_ = 0.25F;
    tick_accumulator_ = 0.0F;
}

void CompletionFireworks::reset_pool() {
    active_count_ = 0;
    free_count_ = max_particles;
    next_birth_order_ = 0;
    for (std::size_t index = 0; index < max_particles; ++index) {
        free_slots_[index] = static_cast<Slot>(max_particles - index - 1);
    }
}

bool CompletionFireworks::append_particle(const Particle& particle) {
    if (free_count_ == 0) {
        return false;
    }
    const Slot slot = free_slots_[--free_count_];
    particles_[slot] = particle;
    birth_order_[slot] = next_birth_order_++;
    active_positions_[slot] = static_cast<Slot>(active_count_);
    active_slots_[active_count_++] = slot;
    return true;
}

void CompletionFireworks::release_particle(const Slot slot) {
    const std::size_t position = active_positions_[slot];
    const Slot moved = active_slots_[--active_count_];
    if (position < active_count_) {
        active_slots_[position] = moved;
        active_positions_[moved] = static_cast<Slot>(position);
    }
    free_slots_[free_count_++] = slot;
}

void CompletionFireworks::release_oldest_particle() {
    std::size_t oldest_position = 0;
    for (std::size_t position = 1; position < active_count_; ++position) {
        if (birth_order_[active_slots_[position]] < birth_order_[active_slots_[oldest_position]]) {
            oldest_position = position;
        }
    }
    release_particle(active_slots_[oldest_position]);
}

void CompletionFireworks::update(const ImVec2 top_left, const ImVec2 size, const float delta_seconds) {
    const float elapsed = std::clamp(delta_seconds, 0.0F, 0.25F);
    next_explosion_ -= elapsed;
    if (next_explosion_ <= 0.0F && size.x > 0.0F && size.y > 0.0F) {
        explode(top_left, size);
        next_explosion_ = explosion_interval_min + random_float(random_, 0.0F, 1.0F) * explosion_interval_jitter;
    }

    tick_accumulator_ += elapsed;
    while (tick_accumulator_ >= tick_seconds) {
        std::array<Slot, max_particles> updating_slots{};
        const std::size_t updating_count = active_count_;
        std::copy_n(active_slots_.begin(), updating_count, updating_slots.begin());
        for (std::size_t index = 0; index < updating_count; ++index) {
            const Slot slot = updating_slots[index];
            auto& particle = particles_[slot];
            particle.x += particle.vx;
            particle.y += particle.vy;
            particle.z += particle.vz;
            particle.vy -= particle_gravity;
            particle.vx *= 0.91F;
            particle.vy *= 0.91F;
            particle.vz *= 0.91F;
            ++particle.age;
            const int age = particle.age;
            if (particle.trail && (age + particle.lifetime) % 4 == 0 &&
                static_cast<float>(age) < static_cast<float>(particle.lifetime) * 0.5F &&
                active_count_ < max_particles) {
                const int trail_lifetime = 24 + random_integer(random_, 0, 5);
                auto trail = particle;
                trail.vx = 0.0F;
                trail.vy = 0.0F;
                trail.vz = 0.0F;
                trail.age = trail_lifetime / 2;
                trail.lifetime = trail_lifetime;
                trail.trail = false;
                append_particle(trail);
            }
            if (particle.age >= particle.lifetime) {
                release_particle(slot);
            }
        }
        tick_accumulator_ -= tick_seconds;
    }
}

void CompletionFireworks::explode(const ImVec2 top_left, const ImVec2 size) {
    constexpr std::array<ImVec4, 10> colors{{
        color(255, 74, 74),    // red
        color(255, 142, 64),   // orange
        color(255, 220, 92),   // gold
        color(128, 224, 112),  // green
        color(72, 208, 188),   // turquoise
        color(78, 178, 255),   // blue
        color(116, 132, 255),  // periwinkle
        color(192, 112, 255),  // purple
        color(255, 116, 204),  // pink
        color(240, 244, 255),  // white
    }};
    const float scale = std::max(1.0F, std::min(size.x / 36.0F, size.y / 18.0F));
    const float world_x = random_float(random_, -12.0F, 12.0F);
    const float world_y = random_float(random_, 5.0F, 14.0F);
    const float world_z = random_float(random_, -2.0F, 2.0F);
    const ImVec2 origin{
        top_left.x + (size.x * 0.5F) + ((world_x - world_z) * scale),
        top_left.y + size.y - (scale * 1.07F) - (world_y * scale) + ((world_x + world_z) * scale * 0.3F),
    };
    std::array<Particle, max_particles> explosion{};
    std::size_t explosion_count = 0;
    const auto commit = [&] {
        // Keep each burst intact; evict the oldest burst instead of truncating this one mid-shape.
        while (active_count_ + explosion_count > max_particles) {
            release_oldest_particle();
        }
        for (std::size_t index = 0; index < explosion_count; ++index) {
            append_particle(explosion[index]);
        }
    };
    const auto pick_color = [&] {
        return colors[random_integer(random_, std::size_t{0}, colors.size() - 1)];
    };
    const auto add_particle = [&](const float vx, const float vy, const float vz, const ImVec4 particle_color,
                                  const bool trail, const bool twinkle) {
        if (explosion_count >= max_particles) {
            return;
        }
        explosion[explosion_count++] = Particle{.origin = origin,
                                                .scale = scale,
                                                .vx = vx,
                                                .vy = vy,
                                                .vz = vz,
                                                .color = particle_color,
                                                .lifetime = 48 + random_integer(random_, 0, 11),
                                                .trail = trail,
                                                .twinkle = twinkle};
    };
    const int shape = random_integer(random_, 0, 4);
    const bool trail = true;
    const bool twinkle = shape % 2 == 0;

    if (shape == 4) {
        for (int index = 0; index < 70; ++index) {
            const float base_off_x = random_gaussian(random_, 0.05F);
            const float base_off_z = random_gaussian(random_, 0.05F);
            add_particle(random_gaussian(random_, 0.15F) + base_off_x, random_float(random_, 0.0F, 0.5F),
                         random_gaussian(random_, 0.15F) + base_off_z, pick_color(), trail, twinkle);
        }
        commit();
        return;
    }

    constexpr std::array<std::array<float, 2>, 6> star{{{0.0F, 1.0F},
                                                        {0.3455F, 0.309F},
                                                        {0.9511F, 0.309F},
                                                        {0.3796F, -0.1265F},
                                                        {0.6122F, -0.8041F},
                                                        {0.0F, -0.3592F}}};
    constexpr std::array<std::array<float, 2>, 12> creeper{{{0.0F, 0.2F},
                                                            {0.2F, 0.2F},
                                                            {0.2F, 0.6F},
                                                            {0.6F, 0.6F},
                                                            {0.6F, 0.2F},
                                                            {0.2F, 0.2F},
                                                            {0.2F, 0.0F},
                                                            {0.4F, 0.0F},
                                                            {0.4F, -0.6F},
                                                            {0.2F, -0.6F},
                                                            {0.2F, -0.4F},
                                                            {0.0F, -0.4F}}};
    if (shape == 2 || shape == 3) {
        const auto coords =
            shape == 3 ? std::span<const std::array<float, 2>>{creeper} : std::span<const std::array<float, 2>>{star};
        add_particle(coords[0][0] * 0.5F, coords[0][1] * 0.5F, 0.0F, pick_color(), trail, twinkle);
        const float base = random_float(random_, 0.0F, pi);
        for (int arm = 0; arm < 3; ++arm) {
            const float angle = base + arm * pi * (shape == 3 ? 0.034F : 0.34F);
            for (std::size_t index = 1; index < coords.size(); ++index) {
                for (float step = 0.25F; step <= 1.0F; step += 0.25F) {
                    const float xa = (coords[index - 1][0] + (coords[index][0] - coords[index - 1][0]) * step) * 0.5F;
                    const float ya = (coords[index - 1][1] + (coords[index][1] - coords[index - 1][1]) * step) * 0.5F;
                    const float vx = xa * std::cos(angle);
                    const float vz = xa * std::sin(angle);
                    add_particle(vx, ya, vz, pick_color(), trail, twinkle);
                    add_particle(-vx, ya, -vz, pick_color(), trail, twinkle);
                }
            }
        }
        commit();
        return;
    }

    const int steps = shape == 1 ? 3 : 2;
    for (int yy = -steps; yy <= steps; ++yy)
        for (int xx = -steps; xx <= steps; ++xx)
            for (int zz = -steps; zz <= steps; ++zz) {
                if (yy != -steps && yy != steps && xx != -steps && xx != steps) {
                    zz += steps * 2 - 1;
                }
                float vx = xx + random_float(random_, 0.0F, 1.0F) - random_float(random_, 0.0F, 1.0F);
                float vy = yy + random_float(random_, 0.0F, 1.0F) - random_float(random_, 0.0F, 1.0F);
                float vz = zz + random_float(random_, 0.0F, 1.0F) - random_float(random_, 0.0F, 1.0F);
                const float length = std::sqrt((vx * vx) + (vy * vy) + (vz * vz)) / (shape == 1 ? 0.5F : 0.25F) +
                                     random_gaussian(random_, 0.05F);
                add_particle(vx / length, vy / length, vz / length, pick_color(), trail, twinkle);
            }

    commit();
}

void CompletionFireworks::draw(ImDrawList* draw, const ImVec2 clip_min, const ImVec2 clip_max) const {
    draw->PushClipRect(clip_min, clip_max, true);
    const float render_dt = tick_accumulator_ / tick_seconds;
    for (std::size_t index = 0; index < active_count_; ++index) {
        const auto& particle = particles_[active_slots_[index]];
        if (particle.twinkle && particle.age > particle.lifetime / 3 &&
            static_cast<int>((particle.age + particle.lifetime) / 3) % 2)
            continue;
        const float fade_start = static_cast<float>(particle.lifetime) * 0.5F;
        const float fade_t = std::clamp((static_cast<float>(particle.age) - fade_start) /
                                            (static_cast<float>(particle.lifetime) - fade_start),
                                        0.0F, 1.0F);
        auto particle_color = lerp_color(particle.color, particle.fade, fade_t);
        particle_color.w = 1.0F - fade_t;
        const float x = particle.x + (particle.vx * render_dt);
        const float y = particle.y + (particle.vy * render_dt);
        const float z = particle.z + (particle.vz * render_dt);
        const auto project = [&](const float px, const float py, const float pz) {
            return ImVec2{particle.origin.x + ((px - pz) * particle.scale),
                          particle.origin.y - (py * particle.scale) + ((px + pz) * particle.scale * 0.3F)};
        };
        const auto current = project(x, y, z);
        const auto tint = ImGui::ColorConvertFloat4ToU32(particle_color);
        if (particle.trail) {
            const auto previous =
                project(x - (particle.vx * 0.25F), y - (particle.vy * 0.25F), z - (particle.vz * 0.25F));
            draw->AddLine(previous, current, tint, 1.0F);
        }
        draw->AddCircleFilled(current, 1.0F + (1.0F - fade_t), tint, 6);
    }
    draw->PopClipRect();
}

}  // namespace beacon
