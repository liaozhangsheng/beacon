#include "desktop_renderer_internal.hpp"

#include <beacon/ui/sizing.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>

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

bool WindowRenderer::Impl::is_stats_group(const LayoutGroup& group, const PublishedState& state,
                                          const std::span<const std::uint32_t> nodes) {
    return group.source == LayoutSource::Nodes && !nodes.empty() && std::ranges::all_of(nodes, [&](const auto node) {
               return node < state.compiled->graph.nodes.size() &&
                      state.compiled->graph.nodes[node].op == RuleOp::Fact &&
                      state.compiled->graph.nodes[node].fact_key.starts_with("stat/");
           });
}

void WindowRenderer::Impl::draw_overlay_layout(const PublishedState& state, const ImVec2 origin, const ImVec2 available,
                                               const float body_height, const float overlay_scroll_speed,
                                               const bool scroll_right) {
    const auto& groups = state.layout->overlay;
    const auto minimum_height = [&](const LayoutGroup& group, const std::span<const std::uint32_t> nodes,
                                    const bool compact_collections, const bool stats) {
        const auto padding = from_icon(group.padding);
        return padding * 2.0F + (compact_collections ? ui::progress_frame_for_icon(icon_size())
                                                     : ui::progress_frame_for_icon(icon_size()) + from_icon(2.0F) +
                                                           (!nodes.empty() ? ImGui::GetFontSize() : 0.0F) +
                                                           (stats ? ImGui::GetFontSize() + from_icon(1.0F) : 0.0F));
    };

    float required_body_height = 1.0F;
    float max_padding = 0.0F;
    for (std::size_t group_index = 0; group_index < groups.size(); ++group_index) {
        const auto& group = groups[group_index];
        const auto& layout_nodes = view_.overlay_groups[group_index];
        const std::span<const std::uint32_t> nodes = layout_nodes;
        const bool compact_collections = group.source == LayoutSource::Children;
        const bool stats = is_stats_group(group, state, layout_nodes);
        required_body_height = std::max(
            required_body_height,
            overlay_body_height_for_content(group, minimum_height(group, nodes, compact_collections, stats), scale_));
        max_padding = std::max(max_padding, group.padding);
    }
    int window_width = 0;
    int window_height = 0;
    SDL_GetWindowSize(context_.window(), &window_width, &window_height);
    const auto window_overhead = std::max(0.0F, static_cast<float>(window_height) - body_height);
    const auto minimum_window_height = std::max(overlay_min_height(max_padding, scale_),
                                                static_cast<int>(std::ceil(window_overhead + required_body_height)));
    int current_minimum_height = 0;
    SDL_GetWindowMinimumSize(context_.window(), nullptr, &current_minimum_height);
    if (current_minimum_height != minimum_window_height) {
        SDL_SetWindowMinimumSize(context_.window(), 0, minimum_window_height);
    }
    if (window_height < minimum_window_height) {
        SDL_SetWindowSize(context_.window(), window_width, minimum_window_height);
    }
    const float layout_body_height = std::max(body_height, required_body_height);
    for (std::size_t group_index = 0; group_index < groups.size(); ++group_index) {
        const auto& group = groups[group_index];
        const auto& layout_nodes = view_.overlay_groups[group_index];
        std::span<const std::uint32_t> nodes = layout_nodes;
        auto* const draw = ImGui::GetWindowDrawList();
        const bool compact_collections = group.source == LayoutSource::Children;
        const bool stats = is_stats_group(group, state, layout_nodes);
        const bool scrollable = !stats;
        const auto metrics = calculate_layout_metrics(group, true, nodes.size(), available.x, layout_body_height,
                                                      origin.x, origin.y, scale_);
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
        const float minimum_group_height = minimum_height(group, nodes, compact_collections, stats);
        const float height = std::max(1.0F, metrics.configured_height);
        const float content_top =
            metrics.y + padding + std::max(0.0F, (metrics.configured_height - minimum_group_height) * 0.5F);
        float strip_offset = 0.0F;
        draw->PushClipRect({metrics.x, metrics.y}, {metrics.x + metrics.width, metrics.y + height}, true);
        ImDrawListSplitter splitter;
        splitter.Split(draw, 4);
        const auto item_count = scrollable ? carousel->items.size() : nodes.size();
        for (std::size_t item = 0; item < item_count; ++item) {
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
    const float status_height = status_footer_height();
    draw_status_footer(origin, available, origin.y + available.y - status_height);
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
        const bool stats = is_stats_group(group, state, nodes);
        const auto metrics =
            calculate_layout_metrics(group, false, nodes.size(), available.x, body_height, origin.x, origin.y, scale_);
        const auto padding = from_icon(group.padding);
        const float height = metrics.configured_height;
        draw->AddRect({metrics.x, metrics.y}, {metrics.x + metrics.width, metrics.y + height},
                      IM_COL32(120, 120, 130, 255), 0.0, from_icon(2.0F));
        const float content_top = metrics.y + padding;
        std::vector<ImVec2> collection_positions;
        if (collection_item) {
            const float collection_vertical_step = from_icon(16.0F) + metrics.gap;
            const float collection_height = std::max(1.0F, metrics.configured_height - (padding * 2.0F));
            const float collection_item_height = ui::progress_frame_for_icon(this->icon_size() * 0.5F);
            collection_positions.resize(nodes.size());
            float group_x = metrics.content_x;
            for (const auto& collection : view_.main_collections[group_index]) {
                const auto parent = collection.parent;
                const auto& members = collection.members;
                const float group_width = metrics.content_width * static_cast<float>(members.size()) /
                                          static_cast<float>(std::max<std::size_t>(1, nodes.size()));
                const auto group_rows =
                    collection_rows(collection_height, collection_item_height, collection_vertical_step);
                const auto header_rows = std::min(
                    group_rows,
                    static_cast<std::size_t>(std::ceil(metrics.collection_header_height / collection_vertical_step)));
                const auto group_columns = (members.size() + header_rows + group_rows - 1) / group_rows;
                const auto& title = view_.labels[parent];
                const auto progress =
                    std::to_string(std::count_if(members.begin(), members.end(),
                                                 [&](const auto member) {
                                                     return state.snapshot.results[nodes[member]].done;
                                                 })) +
                    "/" + std::to_string(members.size());
                std::vector<float> column_widths(group_columns, 0.0F);
                column_widths.front() =
                    std::max({ui::progress_frame_for_icon(icon_size()), ImGui::CalcTextSize(title.c_str()).x,
                              ImGui::CalcTextSize(progress.c_str()).x});
                for (std::size_t member = 0; member < members.size(); ++member) {
                    const auto column = (member + header_rows) / group_rows;
                    column_widths[column] = std::max(
                        column_widths[column],
                        from_icon(19.0F) + ImGui::CalcTextSize(view_.labels[nodes[members[member]]].c_str()).x);
                }
                const float grid_width = std::accumulate(column_widths.begin(), column_widths.end(), 0.0F);
                const float grid_x = group_x + ((group_width - grid_width) * 0.5F);
                const float parent_icon_size = icon_size();
                const float parent_cell_size = ui::progress_frame_for_icon(parent_icon_size);
                const float parent_x = grid_x + ((column_widths.front() - parent_cell_size) * 0.5F);
                const float parent_y = metrics.y + padding;
                draw_node_icon(state, draw, parent, {parent_x, parent_y}, parent_cell_size,
                               {parent_x + ((parent_cell_size - parent_icon_size) * 0.5F),
                                parent_y + ((parent_cell_size - parent_icon_size) * 0.5F)},
                               parent_icon_size, true, true);
                const auto title_y = parent_y + parent_cell_size + from_icon(3.0F);
                draw_progress_text(draw, title, grid_x + (column_widths.front() * 0.5F), title_y,
                                   IM_COL32(235, 235, 240, 255));
                draw_progress_text(draw, progress, grid_x + (column_widths.front() * 0.5F),
                                   title_y + ImGui::GetFontSize(), IM_COL32(235, 235, 240, 255));
                std::partial_sum(column_widths.begin(), column_widths.end(), column_widths.begin());
                for (std::size_t member = 0; member < members.size(); ++member) {
                    const auto slot = member + header_rows;
                    const auto column = slot / group_rows;
                    const auto row = slot % group_rows;
                    const float column_x = grid_x + (column == 0 ? 0.0F : column_widths[column - 1]);
                    collection_positions[members[member]] = {
                        column_x, content_top + (static_cast<float>(row) * collection_vertical_step)};
                }
                group_x += group_width;
            }
        }
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

    const float status_height = status_footer_height();
    const float controls_height = from_icon(40.0F);
    const float footer_height = status_height + controls_height;
    const float footer_top = origin.y + available.y - footer_height;
    const float footer_center_y = footer_top + status_height + (controls_height * 0.5F);
    const float header_left = draw_main_controls(origin, footer_top, status_height);
    ImGui::PushClipRect({header_left, footer_top + status_height}, {origin.x + available.x, origin.y + available.y},
                        true);
    ImGui::SetCursorScreenPos({origin.x, footer_center_y - from_icon(16.0F)});
    draw_header(state);
    ImGui::PopClipRect();
    draw_status_footer(origin, available, footer_top);
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
