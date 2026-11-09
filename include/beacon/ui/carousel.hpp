#pragma once

#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <concepts>
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
// a new group. The predicate is inlined rather than type-erased, since this runs per group every frame.
template <std::predicate<std::uint32_t> Placed>
std::vector<PlacedItem> place_items(const std::span<const std::uint32_t> nodes, Placed&& placed,
                                    const std::span<const std::uint32_t> group_starts) {
    std::vector<PlacedItem> items;
    bool group_start = false;
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        const auto node = nodes[index];
        group_start = group_start || std::ranges::find(group_starts, node) != group_starts.end();
        if (!placed(node))
            continue;
        items.push_back({.index = index, .node = node, .group_start = group_start});
        group_start = false;
    }
    return items;
}

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

struct CarouselGaps {
    // Between neighbouring items.
    float item = 0.0F;
    // Added before an item that starts a group.
    float group = 0.0F;
    // Added where the row wraps back to its first item, in place of a smaller group gap.
    float loop = 0.0F;
};

// Moves the carousel on, once carousel.source holds this frame's placed items.
void advance_carousel(CarouselState& carousel, std::span<const std::uint32_t> nodes, float content_width,
                      float cell_width, CarouselGaps gaps, float speed, float delta_seconds, bool scroll_right);

template <std::predicate<std::uint32_t> Placed>
void update_carousel(CarouselState& carousel, const std::span<const std::uint32_t> nodes, Placed&& placed,
                     const float content_width, const float cell_width,
                     const std::span<const std::uint32_t> group_starts, const CarouselGaps gaps, const float speed,
                     const float delta_seconds, const bool scroll_right = false) {
    carousel.source = place_items(nodes, placed, group_starts);
    advance_carousel(carousel, nodes, content_width, cell_width, gaps, speed, delta_seconds, scroll_right);
}

// A fixed row packs the placed items from the left.
std::vector<CarouselItem> fixed_strip(std::span<const PlacedItem> items, float cell_width, float gap, float group_gap);

}  // namespace beacon
