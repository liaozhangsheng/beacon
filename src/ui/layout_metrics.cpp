#include <beacon/ui/layout_metrics.hpp>
#include <beacon/ui/sizing.hpp>

#include <algorithm>

namespace beacon {

LayoutMetrics calculate_layout_metrics(const LayoutGroup& group, const bool overlay, const std::size_t node_count,
                                       const float available_width, const float body_height, const float origin_x,
                                       const float origin_y, const float scale) {
    const bool collection = group.source == LayoutSource::Children;
    const bool collection_item = !overlay && collection;
    LayoutMetrics result;
    const auto from_icon = [scale](const float original_size) {
        return ui::from_icon(original_size, scale);
    };
    const auto icon_size = ui::icon_size(scale);
    const auto font_size = ui::font_size(scale);
    const auto frame_size = ui::progress_frame_size(scale);
    const auto margin = from_icon(group.margin);
    const auto padding = from_icon(group.padding);
    result.x = origin_x + (group.x * available_width) + margin;
    result.y =
        (overlay ? origin_y + from_icon(8.0F) + (group.y * body_height) : origin_y + (group.y * body_height)) + margin;
    result.width = std::max(1.0F, (group.width * available_width) - (margin * 2.0F));
    result.configured_height = std::max(1.0F, (group.height * body_height) - (margin * 2.0F));
    result.content_x = result.x + padding;
    result.content_width = std::max(1.0F, result.width - (padding * 2.0F));
    result.gap = collection_item ? from_icon(16.0F) : 0.0F;
    result.collection_header_height = from_icon(64.0F) + font_size;
    result.content_height = std::max(1.0F, result.configured_height - (padding * 2.0F) -
                                               (overlay           ? 0.0F
                                                : collection_item ? result.collection_header_height
                                                                  : 0.0F));
    const auto item_width = overlay           ? (collection ? icon_size * 2.0F : frame_size + icon_size)
                            : collection_item ? frame_size
                                              : frame_size + icon_size;
    const auto width_columns = std::max<std::size_t>(
        1, static_cast<std::size_t>((result.content_width + result.gap) / (item_width + result.gap)));
    result.rows = overlay ? 1
                  : collection_item
                      ? std::max<std::size_t>(1, (node_count + width_columns - 1) / width_columns)
                      : std::max<std::size_t>(1, static_cast<std::size_t>(
                                                     (result.content_height + result.gap) /
                                                     (frame_size + from_icon(4.0F) + (font_size * 2.0F) + result.gap)));
    const auto used_columns = overlay ? 1
                              : collection_item
                                  ? width_columns
                                  : std::max<std::size_t>(1, (node_count + result.rows - 1) / result.rows);
    result.cell_width = overlay ? item_width : (result.content_width - result.gap * (used_columns - 1)) / used_columns;
    result.cell_height = overlay ? frame_size : (result.content_height - result.gap * (result.rows - 1)) / result.rows;
    return result;
}

std::size_t collection_rows(const float content_height, const float item_height, const float stride) {
    if (content_height <= item_height || stride <= 0.0F) {
        return 1;
    }
    return 1 + static_cast<std::size_t>((content_height - item_height) / stride);
}

float overlay_body_height_for_content(const LayoutGroup& group, const float content_height, const float scale) {
    if (group.height <= 0.0F) {
        return 0.0F;
    }
    const auto margin = ui::from_icon(group.margin, scale);
    return (content_height + (margin * 2.0F)) / group.height;
}

}  // namespace beacon
