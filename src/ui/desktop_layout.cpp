#include "desktop_renderer_internal.hpp"

#include <beacon/ui/sizing.hpp>

#include <algorithm>
#include <cmath>
#include <span>

namespace beacon {

void WindowRenderer::Impl::prepare_layout(const std::shared_ptr<const PublishedState>& state) {
    if (state && layout_ != state->layout.get()) {
        layout_ = state->layout.get();
        overlay_carousels_.assign(layout_->overlay.size(), {});
        if (overlay_) {
            float max_padding = 0.0F;
            for (const auto& group : layout_->overlay) {
                max_padding = std::max(max_padding, group.padding);
            }
            const auto minimum_height = overlay_min_height(max_padding, scale_);
            SDL_SetWindowMinimumSize(context_.window(), 0, minimum_height);
            int width = 0;
            int height = 0;
            SDL_GetWindowSize(context_.window(), &width, &height);
            if (height != minimum_height) {
                SDL_SetWindowSize(context_.window(), width, minimum_height);
            }
        }
    }
}

void WindowRenderer::Impl::draw_overlay_layout(const PublishedState& state, const ImVec2 origin, const ImVec2 available,
                                               const float body_height, const float overlay_scroll_speed,
                                               const bool scroll_right) {
    const auto& groups = state.layout->overlay;
    float required_body_height = 1.0F;
    float max_padding = 0.0F;
    std::vector<float> minimum_heights;
    minimum_heights.reserve(groups.size());
    for (std::size_t group_index = 0; group_index < groups.size(); ++group_index) {
        const auto& group = groups[group_index];
        minimum_heights.push_back(overlay_group_min_height(group, !view_.overlay_groups[group_index].empty(),
                                                           view_.overlay_stats[group_index], ImGui::GetFontSize(),
                                                           scale_));
        required_body_height =
            std::max(required_body_height, overlay_body_height_for_content(group, minimum_heights.back(), scale_));
        max_padding = std::max(max_padding, group.padding);
    }
    int window_height = 0;
    SDL_GetWindowSize(context_.window(), nullptr, &window_height);
    const auto window_overhead = std::max(0.0F, static_cast<float>(window_height) - body_height);
    context_.ensure_minimum_size(0, std::max(overlay_min_height(max_padding, scale_),
                                             static_cast<int>(std::ceil(window_overhead + required_body_height))));
    const float layout_body_height = std::max(body_height, required_body_height);
    // Full-width groups stacked top to bottom share the spare height equally, so every row gap
    // matches regardless of how the template splits its bands.
    std::vector<LayoutBand> bands;
    if (stacks_full_width(groups)) {
        std::vector<float> contents;
        for (std::size_t group_index = 0; group_index < groups.size(); ++group_index) {
            const auto& group = groups[group_index];
            // Compact rows draw bare icons inside an invisible frame; space them by what is visible.
            const float hidden_frame =
                group.source == LayoutSource::Children ? ui::progress_frame_for_icon(icon_size()) - icon_size() : 0.0F;
            contents.push_back(minimum_heights[group_index] - hidden_frame + (from_icon(group.margin) * 2.0F));
        }
        bands = stack_layout_bands(contents, origin.y + from_icon(8.0F), origin.y + layout_body_height);
    }
    for (std::size_t group_index = 0; group_index < groups.size(); ++group_index) {
        const auto& group = groups[group_index];
        auto metrics = calculate_layout_metrics(group, true, view_.overlay_groups[group_index].size(), available.x,
                                                layout_body_height, origin.x, origin.y, scale_);
        if (!bands.empty()) {
            const auto margin = from_icon(group.margin);
            metrics.y = bands[group_index].y + margin;
            metrics.configured_height = std::max(1.0F, bands[group_index].height - (margin * 2.0F));
        }
        draw_overlay_group(state, group_index, metrics, minimum_heights[group_index], !bands.empty(),
                           overlay_scroll_speed, scroll_right);
    }
    draw_status_footer(origin, available);
}

void WindowRenderer::Impl::draw_overlay_group(const PublishedState& state, const std::size_t group_index,
                                              const LayoutMetrics& metrics, const float minimum_group_height,
                                              const bool stacked, const float overlay_scroll_speed,
                                              const bool scroll_right) {
    const auto& group = state.layout->overlay[group_index];
    const std::span<const std::uint32_t> nodes = view_.overlay_groups[group_index];
    auto* const draw = ImGui::GetWindowDrawList();
    const bool compact_collections = group.source == LayoutSource::Children;
    const bool stats = view_.overlay_stats[group_index];
    const bool scrollable = !stats;
    const float row_gap = metrics.gap;
    const float item_width = metrics.cell_width;
    const auto padding = from_icon(group.padding);
    const auto& collection_group_starts = view_.overlay_collection_starts[group_index];
    const float collection_group_gap = from_icon(48.0F);
    CarouselState* carousel = nullptr;
    if (scrollable) {
        carousel = &overlay_carousels_[group_index];
        update_carousel(
            *carousel, nodes, state.snapshot.results,
            [this](const auto node) {
                return completion_.active(node);
            },
            metrics.content_width, item_width, row_gap, collection_group_starts, collection_group_gap,
            overlay_scroll_speed * (compact_collections ? 1.5F : 1.0F), ImGui::GetIO().DeltaTime, scroll_right);
    }
    if (carousel && !carousel->items.empty() && overlay_scroll_speed != 0.0F) {
        continuous_animation_ = true;
    }
    const float height = std::max(1.0F, metrics.configured_height);
    const float content_top = metrics.y + padding +
                              (stacked ? (metrics.configured_height - minimum_group_height) * 0.5F
                                       : std::max(0.0F, (metrics.configured_height - minimum_group_height) * 0.5F));
    float strip_offset = 0.0F;
    draw->PushClipRect({metrics.x, metrics.y}, {metrics.x + metrics.width, metrics.y + height}, true);
    ImDrawListSplitter splitter;
    splitter.Split(draw, 4);
    const auto item_count = scrollable ? carousel->items.size() : nodes.size();
    for (std::size_t item = 0; item < item_count; ++item) {
        if (scrollable && !carousel->items[item].visible)
            continue;
        const auto index = scrollable ? carousel->items[item].node : nodes[item];
        if (!scrollable && item > 0 &&
            std::ranges::find(collection_group_starts, index) != collection_group_starts.end()) {
            strip_offset += collection_group_gap;
        }
        const float cell_x =
            scrollable ? metrics.content_x + carousel->items[item].x : metrics.content_x + strip_offset;
        if (!scrollable) {
            strip_offset += item_width + row_gap;
        }
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
                   .show_frame = !compact_collections,
                   .draw_glow = stats,
                   .stats = stats,
                   .scrollable = scrollable},
                  splitter);
    }
    splitter.Merge(draw);
    draw->PopClipRect();
}

std::vector<ImVec2> WindowRenderer::Impl::draw_collections(const PublishedState& state, const std::size_t group_index,
                                                           const LayoutMetrics& metrics) {
    const auto& nodes = view_.main_groups[group_index];
    auto* const draw = ImGui::GetWindowDrawList();
    const auto padding = from_icon(state.layout->main[group_index].padding);
    const float content_top = metrics.y + padding;
    const float collection_vertical_step = from_icon(16.0F) + metrics.gap;
    const float collection_height = std::max(1.0F, metrics.configured_height - (padding * 2.0F));
    const float collection_item_height = ui::progress_frame_for_icon(this->icon_size() * 0.5F);
    const auto group_rows = collection_rows(collection_height, collection_item_height, collection_vertical_step);
    const auto header_rows = std::min(
        group_rows, static_cast<std::size_t>(std::ceil(metrics.collection_header_height / collection_vertical_step)));
    std::vector<ImVec2> positions(nodes.size());
    float group_x = metrics.content_x;
    for (const auto& collection : view_.main_collections[group_index]) {
        const auto parent = collection.parent;
        const auto& members = collection.members;
        const float group_width = metrics.content_width * static_cast<float>(members.size()) /
                                  static_cast<float>(std::max<std::size_t>(1, nodes.size()));
        const auto& title = view_.labels[parent];
        const auto progress = std::to_string(std::count_if(members.begin(), members.end(),
                                                           [&](const auto member) {
                                                               return state.snapshot.results[nodes[member]].done;
                                                           })) +
                              "/" + std::to_string(members.size());
        std::vector<float> member_widths;
        member_widths.reserve(members.size());
        for (const auto member : members)
            member_widths.push_back(from_icon(19.0F) + ImGui::CalcTextSize(view_.labels[nodes[member]].c_str()).x);
        const auto grid =
            collection_grid(std::max({ui::progress_frame_for_icon(icon_size()), ImGui::CalcTextSize(title.c_str()).x,
                                      ImGui::CalcTextSize(progress.c_str()).x}),
                            member_widths, group_rows, header_rows, from_icon(16.0F));
        const float grid_x = group_x + ((group_width - grid.width) * 0.5F);
        const float parent_icon_size = icon_size();
        const float parent_cell_size = ui::progress_frame_for_icon(parent_icon_size);
        const float parent_x = grid_x + ((grid.header_width - parent_cell_size) * 0.5F);
        const float parent_y = content_top;
        draw_node_icon(state, draw, parent, parent, {parent_x, parent_y}, parent_cell_size,
                       {parent_x + ((parent_cell_size - parent_icon_size) * 0.5F),
                        parent_y + ((parent_cell_size - parent_icon_size) * 0.5F)},
                       parent_icon_size, true, true);
        const auto title_y = parent_y + parent_cell_size + from_icon(3.0F);
        draw_progress_text(draw, title, grid_x + (grid.header_width * 0.5F), title_y, IM_COL32(235, 235, 240, 255));
        draw_progress_text(draw, progress, grid_x + (grid.header_width * 0.5F), title_y + ImGui::GetFontSize(),
                           IM_COL32(235, 235, 240, 255));
        for (std::size_t member = 0; member < members.size(); ++member) {
            const auto [offset, row] = grid.members[member];
            positions[members[member]] = {grid_x + offset,
                                          content_top + (static_cast<float>(row) * collection_vertical_step)};
        }
        group_x += group_width;
    }
    return positions;
}

void WindowRenderer::Impl::draw_main_layout(const PublishedState& state, const ImVec2 origin, const ImVec2 available,
                                            const float body_height) {
    const auto& groups = state.layout->main;
    for (std::size_t group_index = 0; group_index < groups.size(); ++group_index) {
        const auto& group = groups[group_index];
        const auto& nodes = view_.main_groups[group_index];
        auto* const draw = ImGui::GetWindowDrawList();
        const bool collection_item = group.source == LayoutSource::Children;
        if (collection_item)
            ImGui::PushFont(nullptr, ui::font_size(scale_));
        const bool stats = view_.main_stats[group_index];
        const auto metrics =
            calculate_layout_metrics(group, false, nodes.size(), available.x, body_height, origin.x, origin.y, scale_);
        const auto padding = from_icon(group.padding);
        const float height = metrics.configured_height;
        // Inset each panel so neighbours read as separate slots with a gutter between them.
        const float inset = from_icon(2.0F);
        draw_inset_panel(draw, {metrics.x + inset, metrics.y + inset},
                         {metrics.x + metrics.width - inset, metrics.y + height - inset}, from_icon(2.0F));
        const float content_top = metrics.y + padding;
        const auto collection_positions =
            collection_item ? draw_collections(state, group_index, metrics) : std::vector<ImVec2>{};
        draw->PushClipRect({metrics.x, metrics.y}, {metrics.x + metrics.width, metrics.y + height}, true);
        ImDrawListSplitter splitter;
        splitter.Split(draw, 4);
        for (std::size_t item = 0; item < nodes.size(); ++item) {
            const auto index = nodes[item];
            const auto column = item / metrics.rows;
            const auto row = item % metrics.rows;
            const float cell_x =
                collection_item ? collection_positions[item].x
                                : metrics.content_x + (static_cast<float>(column) * (metrics.cell_width + metrics.gap));
            const float cell_y = collection_item
                                     ? collection_positions[item].y
                                     : content_top + (static_cast<float>(row) * (metrics.cell_height + metrics.gap));
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
                       .stats = stats,
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
