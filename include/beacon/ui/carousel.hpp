#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace beacon {

// A node that takes a slot in its group, in layout order.
struct PlacedItem {
    // Position in the group's node list.
    std::size_t index = 0;
    std::uint32_t node = 0;
    // The first placed member of a collection, which starts after a group gap unless it opens the row.
    bool group_start = false;

    bool operator==(const PlacedItem&) const = default;
};

// Every layout places items through this, so a collection whose leading members are not placed still starts
// a new group.
std::vector<PlacedItem> place_items(std::span<const std::uint32_t> nodes,
                                    const std::function<bool(std::uint32_t)>& placed,
                                    std::span<const std::uint32_t> group_starts);

struct CarouselItem {
    std::uint32_t node = 0;
    float x = 0.0F;
    // Items that stop being placed keep an empty slot until they scroll out of view.
    bool visible = true;
};

struct CarouselState {
    std::vector<PlacedItem> source;
    std::vector<CarouselItem> items;
    std::size_t next = 0;
    bool initialized = false;
    bool direction_right = false;
};

void update_carousel(CarouselState& carousel, std::span<const std::uint32_t> nodes,
                     const std::function<bool(std::uint32_t)>& placed, float content_width, float cell_width, float gap,
                     std::span<const std::uint32_t> group_starts, float group_gap, float speed, float delta_seconds,
                     bool scroll_right = false);

// A fixed row packs the placed items from the left.
std::vector<CarouselItem> fixed_strip(std::span<const PlacedItem> items, float cell_width, float gap, float group_gap);

}  // namespace beacon
