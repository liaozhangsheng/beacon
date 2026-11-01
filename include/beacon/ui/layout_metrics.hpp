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

struct LayoutBand {
    float y = 0.0F;
    float height = 0.0F;
};
// True for two or more full-width groups ordered top to bottom.
bool stacks_full_width(const std::vector<LayoutGroup>& groups);
struct LayoutStack {
    std::vector<float> contents;
    std::vector<float> weights;
    // The band each group occupies; the bands between them are the empty space their y positions leave.
    std::vector<std::size_t> group_bands;
};
// Rows of stacked groups plus empty rows for the space before, between and after them.
LayoutStack stack_layout_rows(std::span<const LayoutGroup> groups, std::span<const float> contents);
// The smallest stack height whose weighted bands each fit their content.
float stack_layout_min_height(std::span<const float> contents, std::span<const float> weights);
// Stacked bands split the height in proportion to their weights; a band whose content needs more keeps its
// content height and the others share the rest by weight.
std::vector<LayoutBand> stack_layout_bands(std::span<const float> contents, std::span<const float> weights, float top,
                                           float bottom);

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
