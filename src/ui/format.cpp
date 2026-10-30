#include <beacon/ui/format.hpp>

#include <algorithm>
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

std::string format_rule_progress(const RuleResult& result) {
    return result.target == 0 ? std::to_string(result.value)
                              : std::to_string(result.value) + "/" + std::to_string(result.target);
}

}  // namespace beacon
