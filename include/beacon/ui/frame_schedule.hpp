#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

namespace beacon {

// Soft effects such as the progress glow and the enchantment glint look smooth at this pace.
inline constexpr std::uint64_t soft_animation_ms = 50;

// Gathers, while a frame is drawn, when anything on it changes next. Each animation asks at its own pace, so
// a slow effect never holds a window at the rate of a fast one, and a still window sleeps.
class FrameSchedule {
public:
    static constexpr std::uint64_t idle = std::numeric_limits<std::uint64_t>::max();

    // Starts gathering for a frame drawn at now_ms.
    void begin(const std::uint64_t now_ms) {
        now_ = now_ms;
        next_ = idle;
    }
    // Something changes at at_ms.
    void at(const std::uint64_t at_ms) {
        next_ = std::min(next_, at_ms);
    }
    // Something moves continuously and needs a frame every period_ms.
    void every(const std::uint64_t period_ms) {
        at(now_ + period_ms);
    }
    // Something steps on multiples of period_ms, like a sprite that changes on a fixed clock.
    void step(const std::uint64_t period_ms) {
        at(((now_ / period_ms) + 1) * period_ms);
    }

    [[nodiscard]] std::uint64_t now() const {
        return now_;
    }
    [[nodiscard]] std::uint64_t next() const {
        return next_;
    }

private:
    std::uint64_t now_ = 0;
    std::uint64_t next_ = idle;
};

}  // namespace beacon
