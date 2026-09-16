#pragma once

#include <beacon/ui/display.hpp>

#include <cstddef>

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

}  // namespace beacon
