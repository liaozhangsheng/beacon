#include <beacon/ui/layout_metrics.hpp>
#include <beacon/ui/sizing.hpp>

#include <algorithm>
#include <numeric>

namespace beacon {

LayoutSpacing group_spacing(const LayoutGroup& group, const float scale) {
    const auto& spacing = group.spacing;
    return {.item = ui::from_icon(spacing.item, scale),
            .row = ui::from_icon(spacing.row, scale),
            .label = ui::from_icon(spacing.label, scale),
            .collection = ui::from_icon(spacing.collection, scale)};
}

PanelRect panel_rect(const LayoutGroup& group, const float origin_x, const float origin_y, const float window_width,
                     const float body_height, const float scale) {
    PanelRect panel{.x = origin_x + group.position[0].at(window_width, scale),
                    .y = origin_y + group.position[1].at(body_height, scale),
                    .width = std::max(1.0F, group.size[0].at(window_width, scale)),
                    .height = std::max(1.0F, group.size[1].at(body_height, scale))};
    const auto padding = ui::from_icon(group.padding, scale);
    panel.content_x = panel.x + padding;
    panel.content_y = panel.y + padding;
    panel.content_width = std::max(1.0F, panel.width - (padding * 2.0F));
    panel.content_height = std::max(1.0F, panel.height - (padding * 2.0F));
    return panel;
}

ItemGrid item_grid(const LayoutGroup& group, const PanelRect& panel, const std::size_t count, const float scale) {
    const auto spacing = group_spacing(group, scale);
    // Each row holds a frame, the label gap and two text lines.
    const auto row_height = ui::progress_frame_size(scale) + spacing.label + (ui::font_size(scale) * 2.0F);
    const auto rows = std::max<std::size_t>(
        1, static_cast<std::size_t>((panel.content_height + spacing.row) / (row_height + spacing.row)));
    const auto columns = std::max<std::size_t>(1, (count + rows - 1) / rows);
    return {.rows = rows,
            .cell_width =
                (panel.content_width - (spacing.item * static_cast<float>(columns - 1))) / static_cast<float>(columns),
            .cell_height =
                (panel.content_height - (spacing.row * static_cast<float>(rows - 1))) / static_cast<float>(rows)};
}

std::size_t collection_rows(const float content_height, const float item_height, const float stride) {
    if (content_height <= item_height || stride <= 0.0F) {
        return 1;
    }
    return 1 + static_cast<std::size_t>((content_height - item_height) / stride);
}

float body_height_for_content(const LayoutGroup& group, const float content_height, const float scale) {
    const auto& height = group.size[1];
    if (height.fraction <= 0.0F)
        return 0.0F;
    return (content_height - (height.pixels * scale)) / height.fraction;
}

float overlay_group_min_height(const LayoutGroup& group, const bool labelled, const bool stats, const float font_size,
                               const float scale) {
    const auto from_icon = [scale](const float original_size) {
        return ui::from_icon(original_size, scale);
    };
    const auto frame_size = ui::progress_frame_for_icon(ui::icon_size(scale));
    const auto padding = from_icon(group.padding);
    const auto label_gap = group_spacing(group, scale).label;
    // Collections draw bare icons in one row without labels.
    return padding * 2.0F + (group.source == LayoutSource::Collections
                                 ? frame_size
                                 : frame_size + label_gap + (labelled ? font_size : 0.0F) +
                                       (stats ? font_size + from_icon(1.0F) : 0.0F));
}

CollectionGrid collection_grid(const float header_width, const std::span<const float> member_widths,
                               const std::size_t row_limit, const std::size_t header_rows, const float column_gap) {
    const auto rows = std::max<std::size_t>(1, row_limit);
    const auto columns = std::max<std::size_t>(1, (member_widths.size() + header_rows + rows - 1) / rows);
    std::vector<float> column_widths(columns, 0.0F);
    column_widths.front() = header_width;
    for (std::size_t member = 0; member < member_widths.size(); ++member) {
        auto& width = column_widths[(member + header_rows) / rows];
        width = std::max(width, member_widths[member]);
    }
    CollectionGrid grid{.header_width = column_widths.front(),
                        .width = std::accumulate(column_widths.begin(), column_widths.end(), 0.0F) +
                                 (column_gap * static_cast<float>(columns - 1)),
                        .members = {}};
    std::partial_sum(column_widths.begin(), column_widths.end(), column_widths.begin());
    grid.members.reserve(member_widths.size());
    for (std::size_t member = 0; member < member_widths.size(); ++member) {
        const auto slot = member + header_rows;
        const auto column = slot / rows;
        grid.members.emplace_back(
            column == 0 ? 0.0F : column_widths[column - 1] + (column_gap * static_cast<float>(column)), slot % rows);
    }
    return grid;
}

}  // namespace beacon
