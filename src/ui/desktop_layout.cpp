#include "desktop_renderer_internal.hpp"

#include <beacon/ui/sizing.hpp>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <span>

namespace beacon {

void WindowRenderer::Impl::prepare_layout(const std::shared_ptr<const PublishedState>& state) {
    if (state && layout_ != state->layout) {
        layout_ = state->layout;
        overlay_carousels_.assign(layout_->overlay.size(), {});
    }
}

void WindowRenderer::Impl::draw_overlay_layout(const PublishedState& state, const ImVec2 origin, const ImVec2 available,
                                               const float body_height, const float overlay_scroll_speed,
                                               const bool scroll_right) {
    const auto& groups = state.layout->overlay;
    float required_body_height = 1.0F;
    for (std::size_t group_index = 0; group_index < groups.size(); ++group_index) {
        const auto metrics =
            calculate_layout_metrics(groups[group_index], true, view_.overlay_groups[group_index].size(), available.x,
                                     body_height, origin.x, origin.y, scale_);
        const auto content_height =
            overlay_group_min_height(groups[group_index], !view_.overlay_groups[group_index].empty(),
                                     view_.overlay_progress[group_index], ImGui::GetFontSize(), scale_);
        required_body_height =
            std::max(required_body_height, body_height_for_content(groups[group_index], content_height, scale_));
        draw_overlay_group(state, group_index, metrics, content_height, overlay_scroll_speed, scroll_right);
    }
    int window_height = 0;
    SDL_GetWindowSize(context_.window(), nullptr, &window_height);
    overlay_min_height_ = std::max(0.0F, static_cast<float>(window_height) - body_height) + required_body_height;
    draw_status_footer(origin, available);
}

void WindowRenderer::Impl::draw_overlay_group(const PublishedState& state, const std::size_t group_index,
                                              const LayoutMetrics& metrics, const float minimum_group_height,
                                              const float overlay_scroll_speed, const bool scroll_right) {
    const auto& group = state.layout->overlay[group_index];
    const std::span<const std::uint32_t> nodes = view_.overlay_groups[group_index];
    auto* const draw = ImGui::GetWindowDrawList();
    const bool compact_collections = group.source == LayoutSource::Collections;
    const bool scrollable = group.scroll;
    const float row_gap = metrics.gap;
    const float item_width = metrics.cell_width;
    const auto padding = from_icon(group.padding);
    const auto& collection_group_starts = view_.overlay_collection_starts[group_index];
    const float collection_group_gap = group_spacing(group, scale_).collection;
    const auto placed = [&](const auto node) {
        return item_placed(state, group, node);
    };
    CarouselState* carousel = nullptr;
    if (scrollable) {
        carousel = &overlay_carousels_[group_index];
        update_carousel(*carousel, nodes, placed, metrics.content_width, item_width, row_gap, collection_group_starts,
                        collection_group_gap, overlay_scroll_speed * (compact_collections ? 1.5F : 1.0F),
                        ImGui::GetIO().DeltaTime, scroll_right);
    }
    if (carousel && !carousel->items.empty() && overlay_scroll_speed != 0.0F) {
        frame_schedule_.every(animation_frame_ms());
    }
    const auto strip = scrollable ? std::vector<CarouselItem>{}
                                  : fixed_strip(place_items(nodes, placed, collection_group_starts), item_width,
                                                row_gap, collection_group_gap);
    const auto& items = scrollable ? carousel->items : strip;
    const float height = std::max(1.0F, metrics.configured_height);
    const float content_top =
        metrics.y + padding + std::max(0.0F, (metrics.configured_height - minimum_group_height) * 0.5F);
    draw->PushClipRect({metrics.x, metrics.y}, {metrics.x + metrics.width, metrics.y + height}, true);
    ImDrawListSplitter splitter;
    splitter.Split(draw, 4);
    for (const auto& item : items) {
        if (!item.visible)
            continue;
        const auto index = item.node;
        const float cell_x = metrics.content_x + item.x;
        const float cell_y = content_top;
        const float icon_size = this->icon_size();
        const float frame_size = ui::progress_frame_for_icon(icon_size);
        const float frame_x = cell_x + ((item_width - frame_size) * 0.5F);
        const float frame_y = cell_y + ((metrics.cell_height - frame_size) * 0.5F);
        const float icon_x = frame_x + ((frame_size - icon_size) * 0.5F);
        const float icon_y = frame_y + ((frame_size - icon_size) * 0.5F);
        draw_item(state, group, draw, index,
                  {.cell_x = cell_x,
                   .cell_y = cell_y,
                   .cell_width = item_width,
                   .cell_height = metrics.cell_height,
                   .frame_x = frame_x,
                   .frame_y = frame_y,
                   .frame_size = frame_size,
                   .icon_x = icon_x,
                   .icon_y = icon_y,
                   .icon_size = icon_size,
                   .show_frame = !compact_collections},
                  splitter);
    }
    splitter.Merge(draw);
    draw->PopClipRect();
}

std::vector<ImVec2> WindowRenderer::Impl::draw_collections(const PublishedState& state, const std::size_t group_index,
                                                           const LayoutMetrics& metrics,
                                                           const std::span<const PlacedItem> placed_items) {
    const auto& nodes = view_.main_groups[group_index];
    std::vector<bool> taken(nodes.size(), false);
    for (const auto& item : placed_items)
        taken[item.index] = true;
    auto* const draw = ImGui::GetWindowDrawList();
    const auto& group = state.layout->main[group_index];
    const auto padding = from_icon(group.padding);
    const auto spacing = group_spacing(group, scale_);
    const float content_top = metrics.y + padding;
    const float collection_item_height = ui::progress_frame_for_icon(this->icon_size() * 0.5F);
    const float collection_vertical_step = collection_item_height + metrics.row_gap;
    const float collection_height = std::max(1.0F, metrics.configured_height - (padding * 2.0F));
    const auto group_rows = collection_rows(collection_height, collection_item_height, collection_vertical_step);
    const auto header_rows = std::min(
        group_rows, static_cast<std::size_t>(std::ceil(metrics.collection_header_height / collection_vertical_step)));
    std::vector<ImVec2> positions(nodes.size());
    const auto& collections = view_.main_collections[group_index];
    const float shared_width =
        std::max(0.0F, metrics.content_width -
                           (spacing.collection * static_cast<float>(std::max<std::size_t>(1, collections.size()) - 1)));
    float group_x = metrics.content_x;
    for (const auto& collection : collections) {
        const auto parent = collection.parent;
        const auto& members = collection.members;
        const float group_width = shared_width * static_cast<float>(members.size()) /
                                  static_cast<float>(std::max<std::size_t>(1, nodes.size()));
        const auto& title = view_.labels[parent];
        const auto progress = std::to_string(std::count_if(members.begin(), members.end(),
                                                           [&](const auto member) {
                                                               return state.snapshot.results[nodes[member]].done;
                                                           })) +
                              "/" + std::to_string(members.size());
        std::vector<std::size_t> placed;
        std::ranges::copy_if(members, std::back_inserter(placed), [&](const auto member) {
            return taken[member];
        });
        std::vector<float> member_widths;
        member_widths.reserve(placed.size());
        for (const auto member : placed) {
            // Reserve the widest stage so the grid does not shift as an item advances.
            float label_width = 0.0F;
            for (const auto variant : item_variants(nodes[member], *state.compiled)) {
                if (variant == nodes[member] || state.compiled->presentation_by_node[variant])
                    label_width = std::max(label_width, ImGui::CalcTextSize(view_.labels[variant].c_str()).x);
            }
            member_widths.push_back(from_icon(19.0F) + label_width);
        }
        const auto grid =
            collection_grid(std::max({ui::progress_frame_for_icon(icon_size()), ImGui::CalcTextSize(title.c_str()).x,
                                      ImGui::CalcTextSize(progress.c_str()).x}),
                            member_widths, group_rows, header_rows, metrics.gap);
        const float grid_x = group_x + ((group_width - grid.width) * 0.5F);
        const float parent_icon_size = icon_size();
        const float parent_cell_size = ui::progress_frame_for_icon(parent_icon_size);
        const float parent_x = grid_x + ((grid.header_width - parent_cell_size) * 0.5F);
        const float parent_y = content_top;
        draw_node_icon(state, draw, parent, parent, {parent_x, parent_y}, parent_cell_size,
                       {parent_x + ((parent_cell_size - parent_icon_size) * 0.5F),
                        parent_y + ((parent_cell_size - parent_icon_size) * 0.5F)},
                       parent_icon_size, true, true);
        const auto title_y = parent_y + parent_cell_size + spacing.label;
        draw_progress_text(draw, title, grid_x + (grid.header_width * 0.5F), title_y, IM_COL32(235, 235, 240, 255));
        draw_progress_text(draw, progress, grid_x + (grid.header_width * 0.5F), title_y + ImGui::GetFontSize(),
                           IM_COL32(235, 235, 240, 255));
        for (std::size_t member = 0; member < placed.size(); ++member) {
            const auto [offset, row] = grid.members[member];
            positions[placed[member]] = {grid_x + offset,
                                         content_top + (static_cast<float>(row) * collection_vertical_step)};
        }
        group_x += group_width + spacing.collection;
    }
    return positions;
}

void WindowRenderer::Impl::draw_main_layout(const PublishedState& state, const ImVec2 origin, const ImVec2 available,
                                            const float body_height) {
    const auto& groups = state.layout->main;
    for (std::size_t group_index = 0; group_index < groups.size(); ++group_index) {
        const auto& group = groups[group_index];
        if (group.source == LayoutSource::Leaderboard) {
            draw_leaderboard_group(group, group_index, origin, available, body_height);
            continue;
        }
        const auto& nodes = view_.main_groups[group_index];
        auto* const draw = ImGui::GetWindowDrawList();
        const bool collection_item = group.source == LayoutSource::Collections;
        if (collection_item)
            ImGui::PushFont(nullptr, ui::font_size(scale_));
        const auto placed = place_items(nodes,
                                        [&](const auto node) {
                                            return item_placed(state, group, node);
                                        },
                                        {});
        const auto metrics =
            calculate_layout_metrics(group, false, placed.size(), available.x, body_height, origin.x, origin.y, scale_);
        const auto padding = from_icon(group.padding);
        const float height = metrics.configured_height;
        // Inset each panel so neighbours read as separate slots with a gutter between them.
        const float inset = from_icon(2.0F);
        draw_inset_panel(draw, {metrics.x + inset, metrics.y + inset},
                         {metrics.x + metrics.width - inset, metrics.y + height - inset}, from_icon(2.0F));
        const float content_top = metrics.y + padding;
        const auto collection_positions =
            collection_item ? draw_collections(state, group_index, metrics, placed) : std::vector<ImVec2>{};
        draw->PushClipRect({metrics.x, metrics.y}, {metrics.x + metrics.width, metrics.y + height}, true);
        ImDrawListSplitter splitter;
        splitter.Split(draw, 4);
        for (std::size_t slot = 0; slot < placed.size(); ++slot) {
            const auto item = placed[slot].index;
            const auto index = placed[slot].node;
            const auto column = slot / metrics.rows;
            const auto row = slot % metrics.rows;
            const float cell_x =
                collection_item ? collection_positions[item].x
                                : metrics.content_x + (static_cast<float>(column) * (metrics.cell_width + metrics.gap));
            const float cell_y =
                collection_item ? collection_positions[item].y
                                : content_top + (static_cast<float>(row) * (metrics.cell_height + metrics.row_gap));
            const float icon_size = collection_item ? this->icon_size() * 0.5F : this->icon_size();
            const float frame_size = ui::progress_frame_for_icon(icon_size);
            const float frame_x = cell_x + ((metrics.cell_width - frame_size) * 0.5F);
            const float icon_x =
                collection_item ? cell_x + (icon_size * 0.5F) : cell_x + ((metrics.cell_width - icon_size) * 0.5F);
            const float icon_y = cell_y + ((frame_size - icon_size) * 0.5F);
            draw_item(state, group, draw, index,
                      {.cell_x = cell_x,
                       .cell_y = cell_y,
                       .cell_width = metrics.cell_width,
                       .cell_height = metrics.cell_height,
                       .frame_x = frame_x,
                       .frame_y = cell_y,
                       .frame_size = frame_size,
                       .icon_x = icon_x,
                       .icon_y = icon_y,
                       .icon_size = icon_size,
                       .draw_glow = !collection_item,
                       .collection_item = collection_item},
                      splitter);
        }
        splitter.Merge(draw);
        draw->PopClipRect();
        if (collection_item)
            ImGui::PopFont();
    }

    draw_main_footer(&state, origin, available);
}

void WindowRenderer::Impl::draw_leaderboard_group(const LayoutGroup& group, const std::size_t group_index,
                                                  const ImVec2 origin, const ImVec2 available,
                                                  const float body_height) {
    const auto metrics =
        calculate_layout_metrics(group, false, 0, available.x, body_height, origin.x, origin.y, scale_);
    const float inset = from_icon(2.0F);
    const ImVec2 max{metrics.x + metrics.width, metrics.y + metrics.configured_height};
    draw_inset_panel(ImGui::GetWindowDrawList(), {metrics.x + inset, metrics.y + inset}, {max.x - inset, max.y - inset},
                     from_icon(2.0F));
    if (leaderboards_ == nullptr)
        return;
    const float padding = from_icon(group.padding);
    ImGui::PushID(static_cast<int>(group_index));
    leaderboard_view_.draw({metrics.x + padding, metrics.y + padding}, {max.x - padding, max.y - padding},
                           group.leaderboard, leaderboards_->get(group.leaderboard), glow_.texture, *assets_,
                           context_.renderer(), ui_language_, frame_schedule_);
    ImGui::PopID();
}

void WindowRenderer::Impl::draw_responsive_layout(const PublishedState& state, const float overlay_scroll_speed,
                                                  const bool scroll_right) {
    const auto origin = ImGui::GetCursorScreenPos();
    const auto available = ImGui::GetContentRegionAvail();
    const float status_height = status_footer_height();
    const float footer_height = status_height + (overlay_ ? 0.0F : from_icon(40.0F));
    const float body_height = std::max(1.0F, available.y - footer_height);
    if (overlay_) {
        draw_overlay_layout(state, origin, available, body_height, overlay_scroll_speed, scroll_right);
    } else {
        draw_main_layout(state, origin, available, body_height);
    }
}

}  // namespace beacon
