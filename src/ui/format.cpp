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

}  // namespace beacon
