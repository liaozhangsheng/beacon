#include <beacon/ui/carousel.hpp>

#include <algorithm>

namespace beacon {

std::vector<PlacedItem> place_items(const std::span<const std::uint32_t> nodes,
                                    const std::function<bool(const std::uint32_t)>& placed,
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

void update_carousel(CarouselState& carousel, const std::span<const std::uint32_t> nodes,
                     const std::function<bool(const std::uint32_t)>& placed, const float content_width,
                     const float cell_width, const float gap, const std::span<const std::uint32_t> group_starts,
                     const float group_gap, const float speed, const float delta_seconds, const bool scroll_right) {
    if (carousel.initialized && carousel.direction_right != scroll_right) {
        carousel.items.clear();
        carousel.next = 0;
        carousel.initialized = false;
    }
    carousel.direction_right = scroll_right;
    carousel.source = place_items(nodes, placed, group_starts);
    // Refill after the node at the tail, wherever it now sits among the placed items.
    const auto tail = carousel.items.empty() ? nodes.end() : std::ranges::find(nodes, carousel.items.back().node);
    if (tail != nodes.end()) {
        const auto tail_index = static_cast<std::size_t>(tail - nodes.begin());
        carousel.next = static_cast<std::size_t>(std::ranges::count_if(carousel.source, [&](const auto& item) {
            return item.index <= tail_index;
        }));
    }
    carousel.next = carousel.source.empty() ? 0 : carousel.next % carousel.source.size();

    const float movement = speed * std::clamp(delta_seconds, 0.0F, 0.25F);
    for (auto& item : carousel.items) {
        item.x += scroll_right ? movement : -movement;
        item.visible = placed(item.node);
    }
    std::erase_if(carousel.items, [&](const auto& item) {
        return scroll_right ? item.x > content_width : item.x + cell_width < 0.0F;
    });
    const float direction = scroll_right ? -1.0F : 1.0F;
    const float stride = direction * (cell_width + gap);
    float next_x = scroll_right ? content_width - cell_width : 0.0F;
    if (carousel.initialized)
        next_x = scroll_right ? -cell_width : content_width;
    if (!carousel.items.empty())
        next_x = carousel.items.back().x + stride;
    while (!carousel.source.empty() &&
           (carousel.items.empty() || (scroll_right ? next_x + cell_width > 0.0F : next_x < content_width))) {
        const auto& item = carousel.source[carousel.next];
        if (!carousel.items.empty() && item.group_start)
            next_x += direction * group_gap;
        carousel.items.push_back({item.node, next_x});
        next_x += stride;
        carousel.next = (carousel.next + 1) % carousel.source.size();
    }
    carousel.initialized = !carousel.source.empty();
}

std::vector<CarouselItem> fixed_strip(const std::span<const PlacedItem> items, const float cell_width, const float gap,
                                      const float group_gap) {
    std::vector<CarouselItem> strip;
    strip.reserve(items.size());
    float x = 0.0F;
    for (const auto& item : items) {
        if (item.group_start && !strip.empty())
            x += group_gap;
        strip.push_back({item.node, x});
        x += cell_width + gap;
    }
    return strip;
}

}  // namespace beacon
