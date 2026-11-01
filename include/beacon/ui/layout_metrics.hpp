#pragma once

#include <beacon/ui/display.hpp>

#include <cstddef>
#include <span>
#include <utility>
#include <vector>

namespace beacon {

// A group's spacing in pixels at scale.
LayoutSpacing group_spacing(const LayoutGroup& group, float scale = 1.0F);

struct LayoutMetrics {
    float x = 0.0F;
    float y = 0.0F;
    float width = 0.0F;
    float configured_height = 0.0F;
    float content_x = 0.0F;
    float content_width = 0.0F;
    float content_height = 0.0F;
    float collection_header_height = 0.0F;
    float gap = 0.0F;
    float row_gap = 0.0F;
    float cell_width = 0.0F;
    std::size_t rows = 1;
    float cell_height = 0.0F;
};

LayoutMetrics calculate_layout_metrics(const LayoutGroup& group, bool overlay, std::size_t node_count,
                                       float available_width, float body_height, float origin_x, float origin_y,
                                       float scale = 1.0F);
std::size_t collection_rows(float content_height, float item_height, float stride);
// The body height at which a group's share fits the given content height.
float body_height_for_content(const LayoutGroup& group, float content_height, float scale = 1.0F);
// The height an overlay group needs for its frames, labels and statistic line before stretching.
float overlay_group_min_height(const LayoutGroup& group, bool labelled, bool stats, float font_size,
                               float scale = 1.0F);
// The height a main-window group needs for one row of items with two label lines.
float main_group_min_height(const LayoutGroup& group, float font_size, float scale = 1.0F);

struct CollectionGrid {
    // The first column starts with the parent's icon, title and progress.
    float header_width = 0.0F;
    float width = 0.0F;
    // Each member's x offset from the grid's left edge, and its row.
    std::vector<std::pair<float, std::size_t>> members;
};
// Fills columns top to bottom, after header_rows slots reserved for the parent in the first column.
// Neighbouring columns are column_gap apart.
CollectionGrid collection_grid(float header_width, std::span<const float> member_widths, std::size_t rows,
                               std::size_t header_rows, float column_gap = 0.0F);

}  // namespace beacon
