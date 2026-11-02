#pragma once

#include <array>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>

namespace beacon::update {

using Version = std::array<std::uint64_t, 3>;

// Accepts exactly MAJOR.MINOR.PATCH without leading zeros or overflowing components.
inline std::optional<Version> parse_version(const std::string_view text) {
    Version result{};
    std::size_t begin = 0;
    for (std::size_t index = 0; index != result.size(); ++index) {
        const auto end = text.find('.', begin);
        if ((index != 2 && end == std::string_view::npos) || (index == 2 && end != std::string_view::npos)) {
            return std::nullopt;
        }
        const auto finish = end == std::string_view::npos ? text.size() : end;
        const auto part = text.substr(begin, finish - begin);
        if (part.empty() || (part.size() > 1 && part.front() == '0')) {
            return std::nullopt;
        }
        const auto parsed = std::from_chars(part.data(), part.data() + part.size(), result[index]);
        if (parsed.ec != std::errc{} || parsed.ptr != part.data() + part.size()) {
            return std::nullopt;
        }
        begin = finish + 1;
    }
    return result;
}

}  // namespace beacon::update
