#pragma once

#include <beacon/core/model.hpp>

#include <cstdint>
#include <string>

namespace beacon {

std::string format_elapsed(std::int64_t elapsed_seconds);
std::string format_percentage(float ratio);
// Minecraft ticks (20 per second) as HH:MM:SS.cc.
std::string format_igt(std::int64_t play_ticks);
// "value/target", or just the value for counters without a target. A unit converts the raw count,
// picking one scale for both numbers from the target (or the value when there is none).
std::string format_rule_progress(const RuleResult& result, Presentation::Unit unit = Presentation::Unit::None);

}  // namespace beacon
