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

LayoutMetrics calculate_layout_metrics(const LayoutGroup& group, const bool overlay, const std::size_t node_count,
                                       const float available_width, const float body_height, const float origin_x,
                                       const float origin_y, const float scale) {
    const bool collection = group.source == LayoutSource::Collections;
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
    const auto spacing = group_spacing(group, scale);
    result.x = origin_x + (group.x * available_width) + margin;
    result.y = origin_y + (group.y * body_height) + margin;
    result.width = std::max(1.0F, (group.width * available_width) - (margin * 2.0F));
    result.configured_height = std::max(1.0F, (group.height * body_height) - (margin * 2.0F));
    result.content_x = result.x + padding;
    result.content_width = std::max(1.0F, result.width - (padding * 2.0F));
    result.gap = spacing.item;
    result.row_gap = spacing.row;
    result.collection_header_height = from_icon(64.0F) + font_size;
    result.content_height = std::max(1.0F, result.configured_height - (padding * 2.0F) -
                                               (overlay           ? 0.0F
                                                : collection_item ? result.collection_header_height
                                                                  : 0.0F));
    const auto item_width = overlay           ? (collection ? icon_size : frame_size + icon_size)
                            : collection_item ? frame_size
                                              : frame_size + icon_size;
    const auto width_columns = std::max<std::size_t>(
        1, static_cast<std::size_t>((result.content_width + result.gap) / (item_width + result.gap)));
    result.rows = overlay ? 1
                  : collection_item
                      ? std::max<std::size_t>(1, (node_count + width_columns - 1) / width_columns)
                      : std::max<std::size_t>(1, static_cast<std::size_t>((result.content_height + result.row_gap) /
                                                                          (frame_size + spacing.label +
                                                                           (font_size * 2.0F) + result.row_gap)));
    const auto used_columns = overlay ? 1
                              : collection_item
                                  ? width_columns
                                  : std::max<std::size_t>(1, (node_count + result.rows - 1) / result.rows);
    result.cell_width = overlay ? item_width : (result.content_width - result.gap * (used_columns - 1)) / used_columns;
    result.cell_height =
        overlay ? frame_size : (result.content_height - result.row_gap * (result.rows - 1)) / result.rows;
    return result;
}

std::size_t collection_rows(const float content_height, const float item_height, const float stride) {
    if (content_height <= item_height || stride <= 0.0F) {
        return 1;
    }
    return 1 + static_cast<std::size_t>((content_height - item_height) / stride);
}

float body_height_for_content(const LayoutGroup& group, const float content_height, const float scale) {
    if (group.height <= 0.0F) {
        return 0.0F;
    }
    const auto margin = ui::from_icon(group.margin, scale);
    return (content_height + (margin * 2.0F)) / group.height;
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

float main_group_min_height(const LayoutGroup& group, const float font_size, const float scale) {
    // Item rows and the collection header both stack a frame, the label gap and two text lines.
    return (ui::from_icon(group.padding, scale) * 2.0F) + ui::progress_frame_size(scale) +
           group_spacing(group, scale).label + (font_size * 2.0F);
}

bool stacks_full_width(const std::vector<LayoutGroup>& groups) {
    return groups.size() > 1 &&
           std::ranges::all_of(groups,
                               [](const LayoutGroup& group) {
                                   return group.x <= 0.0F && group.width >= 1.0F;
                               }) &&
           std::ranges::is_sorted(groups, {}, &LayoutGroup::y);
}

LayoutStack stack_layout_rows(const std::span<const LayoutGroup> groups, const std::span<const float> contents) {
    // Ignore rounding left over from normalising the template's grid units.
    constexpr float min_gap = 1e-4F;
    LayoutStack stack;
    const auto add_gap = [&stack](const float gap) {
        if (gap > min_gap) {
            stack.contents.push_back(0.0F);
            stack.weights.push_back(gap);
        }
    };
    float end = 0.0F;
    for (std::size_t index = 0; index < groups.size(); ++index) {
        add_gap(groups[index].y - end);
        stack.group_bands.push_back(stack.contents.size());
        stack.contents.push_back(contents[index]);
        stack.weights.push_back(groups[index].height);
        end = std::max(end, groups[index].y + groups[index].height);
    }
    add_gap(1.0F - end);
    return stack;
}

float stack_layout_min_height(const std::span<const float> contents, const std::span<const float> weights) {
    const float weight_total = std::accumulate(weights.begin(), weights.end(), 0.0F);
    float height = std::accumulate(contents.begin(), contents.end(), 0.0F);
    for (std::size_t index = 0; index < contents.size(); ++index) {
        if (weights[index] > 0.0F)
            height = std::max(height, contents[index] * weight_total / weights[index]);
    }
    return height;
}

std::vector<LayoutBand> stack_layout_bands(const std::span<const float> contents, const std::span<const float> weights,
                                           const float top, const float bottom) {
    // Pin bands whose content exceeds their weighted share until the remaining shares all fit.
    std::vector<float> heights(contents.size(), 0.0F);
    std::vector<bool> pinned(contents.size(), false);
    for (bool changed = true; changed;) {
        changed = false;
        float free_height = bottom - top;
        float free_weight = 0.0F;
        for (std::size_t index = 0; index < contents.size(); ++index) {
            if (pinned[index])
                free_height -= contents[index];
            else
                free_weight += weights[index];
        }
        for (std::size_t index = 0; index < contents.size(); ++index) {
            if (pinned[index]) {
                heights[index] = contents[index];
                continue;
            }
            heights[index] = free_weight > 0.0F ? std::max(0.0F, free_height) * weights[index] / free_weight : 0.0F;
            if (heights[index] < contents[index]) {
                pinned[index] = true;
                changed = true;
            }
        }
    }
    std::vector<LayoutBand> bands;
    bands.reserve(contents.size());
    float y = top;
    for (const auto height : heights) {
        bands.push_back({.y = y, .height = height});
        y += height;
    }
    return bands;
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
