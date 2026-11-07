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
        const auto panel = panel_rect(groups[group_index], origin.x, origin.y, available.x, body_height, scale_);
        const auto content_height =
            overlay_group_min_height(groups[group_index], !view_.overlay_groups[group_index].empty(),
                                     view_.overlay_progress[group_index], ImGui::GetFontSize(), scale_);
        required_body_height =
            std::max(required_body_height, body_height_for_content(groups[group_index], content_height, scale_));
        draw_overlay_group(state, group_index, panel, content_height, overlay_scroll_speed, scroll_right);
    }
    int window_height = 0;
    SDL_GetWindowSize(context_.window(), nullptr, &window_height);
    overlay_min_height_ = std::max(0.0F, static_cast<float>(window_height) - body_height) + required_body_height;
    draw_status_footer(origin, available);
}

void WindowRenderer::Impl::draw_overlay_group(const PublishedState& state, const std::size_t group_index,
                                              const PanelRect& panel, const float minimum_group_height,
                                              const float overlay_scroll_speed, const bool scroll_right) {
    const auto& group = state.layout->overlay[group_index];
    const std::span<const std::uint32_t> nodes = view_.overlay_groups[group_index];
    auto* const draw = ImGui::GetWindowDrawList();
    const bool compact_collections = group.source == LayoutSource::Collections;
    const bool scrollable = group.scroll;
    const auto spacing = group_spacing(group, scale_);
    const float row_gap = spacing.item;
    const float icon_size = this->icon_size();
    const float frame_size = ui::progress_frame_for_icon(icon_size);
    // Collections draw bare icons; other items reserve their frame plus room for the label.
    const float item_width = compact_collections ? icon_size : frame_size + icon_size;
    const auto& collection_group_starts = view_.overlay_collection_starts[group_index];
    const float collection_group_gap = spacing.collection;
    const auto placed = [&](const auto node) {
        return item_placed(state, group, node);
    };
    CarouselState* carousel = nullptr;
    if (scrollable) {
        carousel = &overlay_carousels_[group_index];
        update_carousel(*carousel, nodes, placed, panel.content_width, item_width, row_gap, collection_group_starts,
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
    const float content_top = panel.content_y + std::max(0.0F, (panel.height - minimum_group_height) * 0.5F);
    const auto clip = scoped_clip(draw, {panel.x, panel.y}, {panel.x + panel.width, panel.y + panel.height});
    ImDrawListSplitter splitter;
    const auto layers = scoped_layers(draw, splitter);
    for (const auto& item : items) {
        if (!item.visible)
            continue;
        const auto index = item.node;
        const float cell_x = panel.content_x + item.x;
        const float frame_x = cell_x + ((item_width - frame_size) * 0.5F);
        const float icon_x = frame_x + ((frame_size - icon_size) * 0.5F);
        const float icon_y = content_top + ((frame_size - icon_size) * 0.5F);
        draw_item(state, group, draw, index,
                  {.cell_x = cell_x,
                   .cell_y = content_top,
                   .cell_width = item_width,
                   .cell_height = frame_size,
                   .frame_x = frame_x,
                   .frame_y = content_top,
                   .frame_size = frame_size,
                   .icon_x = icon_x,
                   .icon_y = icon_y,
                   .icon_size = icon_size,
                   .show_frame = !compact_collections},
                  splitter);
    }
}

std::vector<ImVec2> WindowRenderer::Impl::draw_collections(const PublishedState& state, const std::size_t group_index,
                                                           const PanelRect& panel,
                                                           const std::span<const PlacedItem> placed_items) {
    const auto& nodes = view_.main_groups[group_index];
    std::vector<bool> taken(nodes.size(), false);
    for (const auto& item : placed_items)
        taken[item.index] = true;
    auto* const draw = ImGui::GetWindowDrawList();
    const auto& group = state.layout->main[group_index];
    const auto spacing = group_spacing(group, scale_);
    const float collection_item_height = ui::progress_frame_for_icon(this->icon_size() * 0.5F);
    const float collection_vertical_step = collection_item_height + spacing.row;
    const float collection_height = panel.content_height;
    const auto group_rows = collection_rows(collection_height, collection_item_height, collection_vertical_step);
    // The parent's icon, title and progress take the first column's top slots.
    const float header_height = from_icon(64.0F) + ui::font_size(scale_);
    const auto header_rows =
        std::min(group_rows, static_cast<std::size_t>(std::ceil(header_height / collection_vertical_step)));
    std::vector<ImVec2> positions(nodes.size());
    const auto& collections = view_.main_collections[group_index];
    // Columns fill top to bottom, so a grid's height follows from its member count alone. Every collection in the
    // panel shares the tallest grid's offset, which centers them vertically while keeping their headers level.
    const float parent_icon_size = icon_size();
    const float parent_cell_size = ui::progress_frame_for_icon(parent_icon_size);
    float grid_height = parent_cell_size + spacing.label + (ImGui::GetFontSize() * 2.0F);
    for (const auto& collection : collections) {
        const auto count = static_cast<std::size_t>(std::ranges::count_if(collection.members, [&](const auto member) {
            return taken[member];
        }));
        if (count == 0)
            continue;
        const auto rows = std::min(group_rows, header_rows + count);
        grid_height =
            std::max(grid_height, (static_cast<float>(rows - 1) * collection_vertical_step) + collection_item_height);
    }
    const float grid_top = panel.content_y + std::max(0.0F, (collection_height - grid_height) * 0.5F);
    const float shared_width =
        std::max(0.0F, panel.content_width -
                           (spacing.collection * static_cast<float>(std::max<std::size_t>(1, collections.size()) - 1)));
    float group_x = panel.content_x;
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
                            member_widths, group_rows, header_rows, spacing.item);
        const float grid_x = group_x + ((group_width - grid.width) * 0.5F);
        const float parent_x = grid_x + ((grid.header_width - parent_cell_size) * 0.5F);
        const float parent_y = grid_top;
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
                                         grid_top + (static_cast<float>(row) * collection_vertical_step)};
        }
        group_x += group_width + spacing.collection;
    }
    return positions;
}

void WindowRenderer::Impl::draw_main_layout(const PublishedState& state, const ImVec2 origin, const ImVec2 available,
                                            const float body_height) {
    const auto& groups = state.layout->main;
    auto* const draw = ImGui::GetWindowDrawList();
    for (std::size_t group_index = 0; group_index < groups.size(); ++group_index) {
        const auto& group = groups[group_index];
        const auto panel = panel_rect(group, origin.x, origin.y, available.x, body_height, scale_);
        // Inset each panel so neighbours read as separate slots with a gutter between them.
        const float inset = from_icon(2.0F);
        if (group.frame)
            draw_inset_panel(draw, {panel.x + inset, panel.y + inset},
                             {panel.x + panel.width - inset, panel.y + panel.height - inset}, from_icon(2.0F));
        if (group.source == LayoutSource::Leaderboard) {
            draw_leaderboard_group(group, group_index, panel);
            continue;
        }
        const auto& nodes = view_.main_groups[group_index];
        const bool collection_item = group.source == LayoutSource::Collections;
        const auto font = scoped_font(ui::font_size(scale_), collection_item);
        const auto placed = place_items(nodes,
                                        [&](const auto node) {
                                            return item_placed(state, group, node);
                                        },
                                        {});
        const auto spacing = group_spacing(group, scale_);
        const auto grid = collection_item ? ItemGrid{} : item_grid(group, panel, placed.size(), scale_);
        const auto collection_positions =
            collection_item ? draw_collections(state, group_index, panel, placed) : std::vector<ImVec2>{};
        const float icon_size = collection_item ? this->icon_size() * 0.5F : this->icon_size();
        const float frame_size = ui::progress_frame_for_icon(icon_size);
        // A collection entry's cell starts half an icon before its icon, which the frame centers on.
        const float cell_width = collection_item ? icon_size * 2.0F : grid.cell_width;
        const auto clip = scoped_clip(draw, {panel.x, panel.y}, {panel.x + panel.width, panel.y + panel.height});
        ImDrawListSplitter splitter;
        const auto layers = scoped_layers(draw, splitter);
        for (std::size_t slot = 0; slot < placed.size(); ++slot) {
            const auto column = static_cast<float>(slot / grid.rows);
            const auto row = static_cast<float>(slot % grid.rows);
            const auto cell = collection_item ? collection_positions[placed[slot].index]
                                              : ImVec2{panel.content_x + (column * (grid.cell_width + spacing.item)),
                                                       panel.content_y + (row * (grid.cell_height + spacing.row))};
            draw_item(state, group, draw, placed[slot].node,
                      {.cell_x = cell.x,
                       .cell_y = cell.y,
                       .cell_width = cell_width,
                       .cell_height = grid.cell_height,
                       .frame_x = cell.x + ((cell_width - frame_size) * 0.5F),
                       .frame_y = cell.y,
                       .frame_size = frame_size,
                       .icon_x = cell.x + ((cell_width - icon_size) * 0.5F),
                       .icon_y = cell.y + ((frame_size - icon_size) * 0.5F),
                       .icon_size = icon_size,
                       .draw_glow = !collection_item,
                       .collection_item = collection_item},
                      splitter);
        }
    }

    draw_main_footer(&state, origin, available);
}

void WindowRenderer::Impl::draw_leaderboard_group(const LayoutGroup& group, const std::size_t group_index,
                                                  const PanelRect& panel) {
    if (leaderboards_ == nullptr)
        return;
    const auto id = scoped_id(static_cast<int>(group_index));
    leaderboard_view_.draw({panel.content_x, panel.content_y},
                           {panel.content_x + panel.content_width, panel.content_y + panel.content_height},
                           group.leaderboard, leaderboards_->get(group.leaderboard), glow_texture(), *assets_,
                           context_.renderer(), ui_language_, frame_schedule_);
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
