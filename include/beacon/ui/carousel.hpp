#pragma once

#include <beacon/core/model.hpp>

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace beacon {

struct CarouselItem {
    std::uint32_t node = 0;
    float x = 0.0F;
};

struct CarouselState {
    std::vector<std::uint32_t> source;
    std::vector<CarouselItem> items;
    std::size_t next = 0;
    bool initialized = false;
    bool direction_right = false;
};

void update_carousel(CarouselState& carousel, std::span<const std::uint32_t> nodes, std::span<const RuleResult> results,
                     const std::function<bool(std::uint32_t)>& active, float content_width, float cell_width, float gap,
                     std::span<const std::uint32_t> group_starts, float group_gap, float speed, float delta_seconds,
                     bool scroll_right = false);

}  // namespace beacon
