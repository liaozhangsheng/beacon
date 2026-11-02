#pragma once

// Helpers shared by the updater library and executable.

#include <beacon/core/model.hpp>

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>

namespace beacon::update {

inline Error make_error(const ErrorCode code, std::string message, std::string context = "update") {
    return {.code = code, .message = std::move(message), .context = std::move(context)};
}

// Converts to any ylt::expected<T, Error>, so it can be returned directly.
inline ylt::unexpected<Error> failure(const ErrorCode code, std::string message, std::string context = "update") {
    return ylt::unexpected<Error>{make_error(code, std::move(message), std::move(context))};
}

inline std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

inline bool valid_sha256(const std::string_view value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](const unsigned char character) {
               return std::isxdigit(character) != 0;
           });
}

}  // namespace beacon::update
