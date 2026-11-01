#include "desktop_renderer_internal.hpp"

#include <beacon/ui/format.hpp>
#include <beacon/ui/sizing.hpp>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace beacon {

void WindowRenderer::Impl::update_progress_glows(const PublishedState& state) {
    // Do not skip visible animation phases after a delayed frame or a hidden window.
    const float elapsed = std::clamp(ImGui::GetIO().DeltaTime, 0.0F, 1.0F / 20.0F);
    glow_.time += elapsed;
    const auto target = [&](const std::size_t index) {
        return state.snapshot.results[index].done ? 1.0F : 0.0F;
    };
    if (glow_.brightness.size() != state.snapshot.results.size()) {
        glow_.brightness.resize(state.snapshot.results.size());
        for (std::size_t index = 0; index < glow_.brightness.size(); ++index) {
            glow_.brightness[index] = target(index);
        }
        return;
    }
    const float blend = 1.0F - std::exp(-10.0F * elapsed);
    for (std::size_t index = 0; index < glow_.brightness.size(); ++index) {
        const float next = target(index);
        glow_.brightness[index] =
            next == 0.0F ? 0.0F : glow_.brightness[index] + ((next - glow_.brightness[index]) * blend);
    }
}

void WindowRenderer::Impl::draw_glow(ImDrawList* draw, const ImVec2 frame_min, const float frame_size,
                                     const float brightness, const float phase, const float scale) const {
    if (brightness <= 0.001F || glow_.texture == nullptr) {
        return;
    }
    const float extent = frame_size * scale * 1.42F;
    const ImVec2 center{frame_min.x + (frame_size * 0.5F), frame_min.y + (frame_size * 0.5F)};
    const auto clip_min = draw->GetClipRectMin();
    const auto clip_max = draw->GetClipRectMax();
    if (center.x + extent < clip_min.x || center.x - extent > clip_max.x || center.y + extent < clip_min.y ||
        center.y - extent > clip_max.y)
        return;
    frame_schedule_.every(soft_animation_ms);
    const double time = glow_.time;
    const double angle = (time * 0.25) + phase;
    const float half_size = frame_size * scale;
    const float pulse = 0.6F + (0.55F * (0.5F + 0.5F * static_cast<float>(std::sin((time * 2.4) + phase))));
    const ImVec2 right{static_cast<float>(std::cos(angle)) * half_size,
                       static_cast<float>(std::sin(angle)) * half_size};
    const ImVec2 down{-right.y, right.x};
    draw->AddImageQuad(glow_.texture, {center.x - right.x - down.x, center.y - right.y - down.y},
                       {center.x + right.x - down.x, center.y + right.y - down.y},
                       {center.x + right.x + down.x, center.y + right.y + down.y},
                       {center.x - right.x + down.x, center.y - right.y + down.y}, {0, 0}, {1, 0}, {1, 1}, {0, 1},
                       IM_COL32(255, 255, 255, static_cast<int>(220.0F * brightness * pulse)));
}

void WindowRenderer::Impl::draw_progress_glow(ImDrawList* draw, const std::uint32_t node, const ImVec2 frame_min,
                                              const float icon_size) const {
    const float brightness = node < glow_.brightness.size() ? glow_.brightness[node] : 0.0F;
    draw_glow(draw, frame_min, ui::progress_frame_for_icon(icon_size), brightness,
              static_cast<float>(node) * 2.39996323F);
}

void WindowRenderer::Impl::render_progress_view(const std::shared_ptr<const PublishedState>& state,
                                                const std::optional<Error>& runtime_error, const float scroll_speed,
                                                const bool scroll_right) {
    status_.message.clear();
    status_.color = warning_text_color;
    if (!overlay_ && SDL_GetTicks() < status_.feedback_until) {
        status_.message = status_.feedback;
        status_.color = status_.feedback_color;
    } else if (runtime_error) {
        status_.message = runtime_error->message + ": " + runtime_error->context;
    } else if (update_notice_ != nullptr && !update_notice_->error.empty()) {
        status_.message = update_notice_->error;
        status_.color = error_text_color;
    }
    if (!state) {
        const auto origin = ImGui::GetCursorScreenPos();
        const auto available = ImGui::GetContentRegionAvail();
        if (!overlay_) {
            if (status_.message.empty())
                status_.message = text("未找到 Minecraft 存档", "No Minecraft save found");
            draw_main_footer(nullptr, origin, available);
        } else {
            draw_status_footer(origin, available);
        }
        return;
    }
    if (status_.message.empty() && state->error && state->error->code != ErrorCode::NoSave) {
        status_.message = state->error->message + ": " + state->error->context;
        status_.color = error_text_color;
    }
    if (status_.message.empty() && state->data_status == DataStatus::Stale) {
        status_.message =
            text("进度已过期，正在显示上次成功读取的数据", "Progress is stale; showing the last successful read");
    } else if (status_.message.empty() && !state->has_data() && !overlay_) {
        status_.message = text("未找到 Minecraft 存档", "No Minecraft save found");
    }
    if (status_.message.empty() && !state->warnings.empty()) {
        const auto& warning = state->warnings.front();
        status_.message = std::string(text("存档扫描警告 (", "Save scan warnings (")) +
                          std::to_string(state->warnings.size()) + "): " + warning.message + ": " + warning.context;
    }
    update_progress_glows(*state);
    if (overlay_) {
        if (completion_view_) {
            draw_completion_view(*state);
            const auto origin = ImGui::GetCursorScreenPos();
            const auto available = ImGui::GetContentRegionAvail();
            draw_status_footer(origin, available);
            return;
        }
        draw_header(*state, make_header(*state));
    }
    draw_responsive_layout(*state, scroll_speed, scroll_right);
}

void WindowRenderer::Impl::set_completion_view(const bool complete) {
    if (complete == completion_view_) {
        return;
    }
    completion_view_ = complete;
    fireworks_.reset();
}

void WindowRenderer::Impl::draw_completion_view(const PublishedState& state) {
    auto* const draw = ImGui::GetWindowDrawList();
    const auto window_min = ImGui::GetWindowPos();
    const auto window_size = ImGui::GetWindowSize();
    const auto window_max = ImVec2{window_min.x + window_size.x, window_min.y + window_size.y};
    if (auto* const background = assets_->widget("advancement.png"); background != nullptr) {
        SDL_SetTextureScaleMode(background, SDL_SCALEMODE_NEAREST);
        draw->AddImage(background, window_min, window_max);
    }
    fireworks_.update(window_min, window_size, ImGui::GetIO().DeltaTime);
    fireworks_.draw(draw, window_min, window_max);

    const float avatar_size = std::max(1.0F, window_size.y * 0.5F);
    const auto avatar_min =
        ImVec2{window_min.x + (window_size.y * 0.265F), window_min.y + ((window_size.y - avatar_size) * 0.5F)};
    if (avatar_texture_ != nullptr) {
        if (is_complete(state)) {
            draw_glow(draw, avatar_min, avatar_size, 1.0F, 0.0F, 1.7F);
        }
        draw->AddImage(avatar_texture_, avatar_min, {avatar_min.x + avatar_size, avatar_min.y + avatar_size});
    }

    const auto template_name = std::string(state.localization->text(state.compiled->template_name_key));
    const auto igt =
        std::string("IGT: ") + format_igt(state.snapshot.completion_play_ticks.value_or(state.snapshot.play_ticks));
    const float completion_font_size = std::max(1.0F, window_size.y * 0.25F);
    auto* const font = ImGui::GetFont();
    const float text_x = avatar_min.x + avatar_size + (window_size.y * 0.1F);
    const float line_gap = completion_font_size * 0.2F;
    const float text_block_height = (completion_font_size * 2.0F) + line_gap;
    const float title_y = window_min.y + ((window_size.y - text_block_height) * 0.5F);
    const float igt_y = title_y + completion_font_size + line_gap;
    draw_shadowed_text(draw, font, completion_font_size, {text_x, title_y}, IM_COL32(255, 215, 0, 255),
                       template_name.c_str());
    draw_shadowed_text(draw, font, completion_font_size, {text_x, igt_y}, IM_COL32(255, 215, 0, 255), igt.c_str());
}

void WindowRenderer::Impl::draw_node_icon(const PublishedState& state, ImDrawList* draw, const std::uint32_t node,
                                          const std::uint32_t display_node, const ImVec2 frame_position,
                                          const float frame_size, const ImVec2 icon_position, const float icon_size,
                                          const bool show_frame, const bool draw_glow, const ImU32 icon_tint,
                                          ImDrawListSplitter* splitter) const {
    if (node >= state.snapshot.results.size() || display_node >= node_sprites_.size() ||
        display_node >= state.compiled->presentation_by_node.size() ||
        !state.compiled->presentation_by_node[display_node]) {
        return;
    }
    const auto& sprites = node_sprites_[display_node];
    // The item keeps its own frame while its icon comes from a stage or alternative.
    const auto has_frame = [](const NodeSprites& candidate) {
        return candidate.frame_obtained.texture != nullptr || !candidate.frame_obtained.animation.empty();
    };
    const auto& frames = node < node_sprites_.size() && has_frame(node_sprites_[node]) ? node_sprites_[node] : sprites;
    const auto visible = [&](const ImVec2 position, const float size) {
        const auto clip_min = draw->GetClipRectMin();
        const auto clip_max = draw->GetClipRectMax();
        return position.x + size >= clip_min.x && position.x <= clip_max.x && position.y + size >= clip_min.y &&
               position.y <= clip_max.y;
    };
    const auto draw_sprite = [&](const Sprite& sprite, const ImVec2 position, const float size,
                                 const ImU32 tint = IM_COL32(255, 255, 255, 255), const bool fallback = false) {
        if (!visible(position, size))
            return;
        auto* const texture =
            sprite.animation.empty() ? sprite.texture : assets_->animated_frame(sprite.animation, SDL_GetTicks());
        if (texture != nullptr)
            draw->AddImage(texture, position, {position.x + size, position.y + size}, sprite.uv_min, sprite.uv_max,
                           tint);
        else if (fallback)
            draw_missing_icon(draw, position, size, tint);
    };
    if (show_frame) {
        if (splitter)
            splitter->SetCurrentChannel(draw, 0);
        draw_sprite(state.snapshot.results[node].done ? frames.frame_obtained : frames.frame_unobtained, frame_position,
                    frame_size);
    }
    if (draw_glow) {
        if (splitter)
            splitter->SetCurrentChannel(draw, 1);
        draw_progress_glow(draw, node, frame_position, icon_size);
    }
    if (splitter)
        splitter->SetCurrentChannel(draw, 2);
    draw_sprite(sprites.icon, icon_position, icon_size, icon_tint, true);
    if (sprites.glint.empty() || !visible(icon_position, icon_size))
        return;
    if (auto* const glint = glint_->overlay(sprites.glint, SDL_GetTicks()); glint != nullptr) {
        frame_schedule_.step(EnchantmentGlint::frame_ms);
        // The icon tint also dims the glint on muted entries.
        draw->AddImage(glint, icon_position, {icon_position.x + icon_size, icon_position.y + icon_size}, {0.0F, 0.0F},
                       {1.0F, 1.0F}, icon_tint);
    }
}

void WindowRenderer::Impl::draw_item(const PublishedState& state, const LayoutGroup& group, ImDrawList* draw,
                                     const std::uint32_t index, const ItemLayout& item, ImDrawListSplitter& splitter) {
    if (index >= state.snapshot.results.size() || index >= view_.labels.size()) {
        return;
    }
    const auto& result = state.snapshot.results[index];
    const auto now = SDL_GetTicks();
    const auto shown = item_display(index, *state.compiled, state.snapshot.results, now);
    if (shown.cycling)
        frame_schedule_.step(item_cycle_ms);
    const bool progress = shows_progress(shown.value, *state.compiled);
    // Overlay statistics glow once reached; other overlay items get the completion cover.
    const bool glow = item.draw_glow || (overlay_ && progress);
    // Item styles only apply to collection entries; everything else draws plainly.
    static constexpr ItemStyle plain{};
    const auto& style = !item.collection_item ? plain : result.done ? group.item_style.done : group.item_style.pending;
    if (style.blank)
        return;
    const auto hit_min = item.show_frame ? ImVec2{item.frame_x, item.frame_y} : ImVec2{item.icon_x, item.icon_y};
    const auto hit_size = item.show_frame ? item.frame_size : item.icon_size;
    if (!overlay_ && manual_runtime_ != nullptr && ImGui::GetIO().KeyCtrl &&
        ImGui::IsMouseHoveringRect(hit_min, {hit_min.x + hit_size, hit_min.y + hit_size})) {
        const bool left = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        if (left || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            const auto target = item_manual_node(index, left, *state.compiled, state.snapshot.results, now);
            if (target)
                (void)manual_runtime_->manual_operation(*target, left);
        }
    }
    // Muted collection entries stay legible but recede: gray text, icon at roughly 40% brightness.
    const bool muted = style.emphasis == ItemEmphasis::Muted;
    const auto foreground = muted ? IM_COL32(128, 128, 128, 255) : IM_COL32(245, 245, 245, 255);
    draw_node_icon(state, draw, index, shown.icon, {item.frame_x, item.frame_y}, item.frame_size,
                   {item.icon_x, item.icon_y}, item.icon_size, item.show_frame, glow,
                   muted ? IM_COL32(100, 100, 100, 255) : IM_COL32_WHITE, &splitter);
    splitter.SetCurrentChannel(draw, 3);

    const auto& label = view_.labels[shown.label];
    if (group.source != LayoutSource::Collections) {
        const float label_gap = group_spacing(group, scale_).label;
        const float label_y = (overlay_ ? item.cell_y + item.cell_height : item.cell_y + item.frame_size) + label_gap;
        const float title_wrap_width = item.frame_size + item.icon_size;
        auto line_count = draw_progress_text(draw, label, item.cell_x + (item.cell_width * 0.5F), label_y, foreground,
                                             title_wrap_width);
        if (progress) {
            const auto progress = format_rule_progress(state.snapshot.results[shown.value]);
            draw_progress_text(draw, progress, item.cell_x + (item.cell_width * 0.5F),
                               label_y + (ImGui::GetFontSize() * line_count), foreground);
            ++line_count;
        }
        if (item.scrollable && result.done) {
            const float cover_width = item.frame_size + item.icon_size;
            completion_.draw_cover(
                draw, index, {item.cell_x + ((item.cell_width - cover_width) * 0.5F), item.cell_y},
                {cover_width, item.cell_height + label_gap + from_icon(4.0F) + (ImGui::GetFontSize() * line_count)});
        }
    } else if (item.collection_item) {
        const auto label_y = item.cell_y + ((item.frame_size - ImGui::GetFontSize()) * 0.5F);
        const auto label_x = item.cell_x + (item.icon_size * 1.5F) + from_icon(6.0F);
        draw_progress_text(draw, label, label_x + (ImGui::CalcTextSize(label.c_str()).x * 0.5F), label_y, foreground);
        if (style.strikethrough)
            draw_item_strikethrough(draw, {item.icon_x, item.icon_y}, item.icon_size, {label_x, label_y}, label,
                                    foreground, from_icon(1.0F));
    } else if (item.scrollable && result.done) {
        completion_.draw_cover(draw, index, {item.icon_x, item.icon_y}, {item.icon_size, item.icon_size});
    }
}

}  // namespace beacon
