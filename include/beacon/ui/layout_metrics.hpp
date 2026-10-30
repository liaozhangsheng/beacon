#pragma once

#include <beacon/ui/display.hpp>

#include <cstddef>
#include <span>
#include <utility>
#include <vector>

namespace beacon {

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
    float cell_width = 0.0F;
    std::size_t rows = 1;
    float cell_height = 0.0F;
};

LayoutMetrics calculate_layout_metrics(const LayoutGroup& group, bool overlay, std::size_t node_count,
                                       float available_width, float body_height, float origin_x, float origin_y,
                                       float scale = 1.0F);
std::size_t collection_rows(float content_height, float item_height, float stride);
float overlay_body_height_for_content(const LayoutGroup& group, float content_height, float scale = 1.0F);
// The height an overlay group needs for its frames, labels and statistic line before stretching.
float overlay_group_min_height(const LayoutGroup& group, bool labelled, bool stats, float font_size,
                               float scale = 1.0F);

struct LayoutBand {
    float y = 0.0F;
    float height = 0.0F;
};
// True for two or more full-width groups ordered top to bottom.
bool stacks_full_width(const std::vector<LayoutGroup>& groups);
// Stacked bands keep their content height and share the spare height between top and bottom equally.
std::vector<LayoutBand> stack_layout_bands(std::span<const float> contents, float top, float bottom);

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
