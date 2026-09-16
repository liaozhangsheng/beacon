#include <beacon/ui/carousel.hpp>

#include <algorithm>

namespace beacon {

void update_carousel(CarouselState& carousel, const std::span<const std::uint32_t> nodes,
                     const std::span<const RuleResult> results, const std::function<bool(const std::uint32_t)>& active,
                     const float content_width, const float cell_width, const float gap,
                     const std::span<const std::uint32_t> group_starts, const float group_gap, const float speed,
                     const float delta_seconds, const bool scroll_right) {
    if (carousel.initialized && carousel.direction_right != scroll_right) {
        carousel.items.clear();
        carousel.next = 0;
        carousel.initialized = false;
    }
    carousel.direction_right = scroll_right;
    carousel.source.clear();
    for (const auto node : nodes) {
        if (node < results.size() && (!results[node].done || active(node))) {
            carousel.source.push_back(node);
        }
    }
    carousel.next = carousel.source.empty() ? 0 : carousel.next % carousel.source.size();

    const float movement = speed * std::clamp(delta_seconds, 0.0F, 0.25F);
    for (auto& item : carousel.items) {
        item.x += scroll_right ? movement : -movement;
    }
    std::erase_if(carousel.items, [&](const auto& item) {
        return (scroll_right ? item.x > content_width : item.x + cell_width < 0.0F) ||
               (item.node < results.size() && results[item.node].done && !active(item.node));
    });
    if (!carousel.source.empty() && !carousel.items.empty()) {
        if (const auto it = std::ranges::find(carousel.source, carousel.items.back().node);
            it != carousel.source.end()) {
            carousel.next =
                (static_cast<std::size_t>(std::distance(carousel.source.begin(), it)) + 1) % carousel.source.size();
        }
    }

    const float direction = scroll_right ? -1.0F : 1.0F;
    const float stride = direction * (cell_width + gap);
    float next_x = scroll_right ? content_width - cell_width : 0.0F;
    if (carousel.initialized)
        next_x = scroll_right ? -cell_width : content_width;
    if (!carousel.items.empty())
        next_x = carousel.items.back().x + stride;
    while (!carousel.source.empty() &&
           (carousel.items.empty() || (scroll_right ? next_x + cell_width > 0.0F : next_x < content_width))) {
        const auto node = carousel.source[carousel.next];
        if (!carousel.items.empty() && std::ranges::find(group_starts, node) != group_starts.end())
            next_x += direction * group_gap;
        carousel.items.push_back({node, next_x});
        next_x += stride;
        carousel.next = (carousel.next + 1) % carousel.source.size();
    }
    carousel.initialized = !carousel.source.empty();
}

}  // namespace beacon
