#include <beacon/ui/format.hpp>

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <format>

namespace beacon {

std::string format_elapsed(const std::int64_t elapsed_seconds) {
    const auto elapsed = std::max<std::int64_t>(0, elapsed_seconds);
    if (elapsed < 60) {
        return std::format("{}s", elapsed);
    }
    const auto seconds = elapsed % 60;
    if (elapsed < 3600) {
        return std::format("{}m {:02}s", elapsed / 60, seconds);
    }
    const auto minutes = (elapsed / 60) % 60;
    return std::format("{}h {:02}m {:02}s", elapsed / 3600, minutes, seconds);
}

std::string format_percentage(const float ratio) {
    return std::format("{:.1f}%", ratio * 100.0F);
}

std::string format_igt(const std::int64_t play_ticks) {
    const auto ticks = std::max<std::int64_t>(0, play_ticks);
    const auto seconds = ticks / 20;
    const auto hours = seconds / 3600;
    const auto minutes = (seconds / 60) % 60;
    const auto remainder = seconds % 60;
    const auto centiseconds = (ticks % 20) * 5;
    return std::format("{:02}:{:02}:{:02}.{:02}", hours, minutes, remainder, centiseconds);
}

namespace {

constexpr std::int64_t ticks_per_hour = 20 * 3600;

// Whole units, or tenths when one_decimal; truncated so progress never reads as reached early.
std::string scaled(const std::int64_t value, const std::int64_t divisor, const bool one_decimal) {
    if (!one_decimal)
        return std::to_string(value / divisor);
    const auto tenths = value / (divisor / 10);
    return std::format("{}.{}", tenths / 10, tenths % 10);
}

std::string clock(const std::int64_t ticks, const bool hours) {
    const auto seconds = ticks / 20;
    if (hours)
        return std::format("{}:{:02}:{:02}", seconds / 3600, (seconds / 60) % 60, seconds % 60);
    return std::format("{}:{:02}", seconds / 60, seconds % 60);
}

}  // namespace

std::string format_rule_progress(const RuleResult& result, const Presentation::Unit unit) {
    const auto value = std::max<std::int64_t>(0, result.value);
    const auto reference = result.target != 0 ? result.target : value;
    const auto pair = [&](const auto& format, const std::string_view suffix) {
        auto text = format(value);
        if (result.target != 0)
            text += "/" + format(result.target);
        return text + std::string(suffix);
    };
    switch (unit) {
        case Presentation::Unit::Distance:
            if (reference < 100)
                return pair(
                    [](const auto cm) {
                        return std::to_string(cm);
                    },
                    " cm");
            if (reference < 100'000)
                return pair(
                    [](const auto cm) {
                        return scaled(cm, 100, false);
                    },
                    " m");
            return pair(
                [](const auto cm) {
                    return scaled(cm, 100'000, true);
                },
                " km");
        case Presentation::Unit::Time: {
            const bool hours = reference >= ticks_per_hour;
            return pair(
                [hours](const auto ticks) {
                    return clock(ticks, hours);
                },
                "");
        }
        case Presentation::Unit::Damage:
            // Tenths of a health point; a heart is two points.
            return pair(
                [](const auto tenths) {
                    return scaled(tenths, 20, true);
                },
                " \u2665");
        default:
            return pair(
                [](const auto count) {
                    return std::to_string(count);
                },
                "");
    }
}

}  // namespace beacon
