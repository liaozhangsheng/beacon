#pragma once

#include <cstdint>
#include <string>

namespace beacon {

std::string format_elapsed(std::int64_t elapsed_seconds);
std::string format_percentage(float ratio);

}  // namespace beacon
