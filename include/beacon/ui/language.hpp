#pragma once

#include <string_view>

namespace beacon {

inline const char* interface_text(std::string_view language, const char* chinese, const char* english) {
    return language == "en" ? english : chinese;
}

}  // namespace beacon
