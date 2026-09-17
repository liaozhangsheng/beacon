#include "desktop_renderer_internal.hpp"

#include <beacon/io/file.hpp>
#include <beacon/ui/format.hpp>
#include <beacon/ui/sizing.hpp>

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <limits>

namespace beacon {
namespace {

std::string format_igt(const std::int64_t play_ticks) {
    const auto ticks = std::max<std::int64_t>(0, play_ticks);
    const auto seconds = ticks / 20;
    const auto hours = seconds / 3600;
    const auto minutes = (seconds / 60) % 60;
    const auto remainder = seconds % 60;
    const auto centiseconds = (ticks % 20) * 5;
    return std::format("{:02}:{:02}:{:02}.{:02}", hours, minutes, remainder, centiseconds);
}

std::string format_rule_progress(const RuleResult& result) {
    return result.target == 0 ? std::to_string(result.value)
                              : std::to_string(result.value) + "/" + std::to_string(result.target);
}

}  // namespace

void WindowRenderer::Impl::update_progress_glows(const PublishedState& state) {
    const auto target = [&](const std::size_t index) {
        return state.snapshot.results[index].done ? 1.0F : 0.0F;
    };
    if (glow_brightness_.size() != state.snapshot.results.size()) {
        glow_brightness_.resize(state.snapshot.results.size());
        for (std::size_t index = 0; index < glow_brightness_.size(); ++index) {
            glow_brightness_[index] = target(index);
        }
        glow_animation_time_ = static_cast<float>(ImGui::GetTime());
        glow_update_elapsed_ = 0.0F;
        return;
    }
    glow_update_elapsed_ += std::clamp(ImGui::GetIO().DeltaTime, 0.0F, 0.25F);
    constexpr float glow_update_interval = 1.0F / 30.0F;
    if (glow_update_elapsed_ < glow_update_interval) {
        return;
    }
    const float elapsed = glow_update_elapsed_;
    glow_update_elapsed_ = 0.0F;
    glow_animation_time_ = static_cast<float>(ImGui::GetTime());
    const float blend = 1.0F - std::exp(-10.0F * elapsed);
    for (std::size_t index = 0; index < glow_brightness_.size(); ++index) {
        const float next = target(index);
        glow_brightness_[index] =
            next == 0.0F ? 0.0F : glow_brightness_[index] + ((next - glow_brightness_[index]) * blend);
    }
}

void WindowRenderer::Impl::draw_glow(ImDrawList* draw, const ImVec2 frame_min, const float frame_size,
                                     const float brightness, const float phase, const float scale) const {
    if (brightness <= 0.001F) {
        return;
    }
    if (glow_texture_ == nullptr) {
        return;
    }
    const float extent = frame_size * scale * 1.42F;
    const ImVec2 midpoint{frame_min.x + frame_size * 0.5F, frame_min.y + frame_size * 0.5F};
    const auto clip_min = draw->GetClipRectMin();
    const auto clip_max = draw->GetClipRectMax();
    if (midpoint.x + extent < clip_min.x || midpoint.x - extent > clip_max.x || midpoint.y + extent < clip_min.y ||
        midpoint.y - extent > clip_max.y)
        return;
    continuous_animation_ = true;
    const float time = glow_animation_time_;
    const ImVec2 center{frame_min.x + (frame_size * 0.5F), frame_min.y + (frame_size * 0.5F)};
    const float angle = (time * 0.25F) + phase;
    const float half_size = frame_size * scale;
    const float pulse = 0.6F + (0.55F * (0.5F + 0.5F * std::sin((time * 2.4F) + phase)));
    const ImVec2 right{std::cos(angle) * half_size, std::sin(angle) * half_size};
    const ImVec2 down{-right.y, right.x};
    draw->AddImageQuad(glow_texture_, {center.x - right.x - down.x, center.y - right.y - down.y},
                       {center.x + right.x - down.x, center.y + right.y - down.y},
                       {center.x + right.x + down.x, center.y + right.y + down.y},
                       {center.x - right.x + down.x, center.y - right.y + down.y}, {0, 0}, {1, 0}, {1, 1}, {0, 1},
                       IM_COL32(255, 255, 255, static_cast<int>(220.0F * brightness * pulse)));
}

void WindowRenderer::Impl::draw_progress_glow(ImDrawList* draw, const std::uint32_t node, const ImVec2 frame_min,
                                              const float icon_size) const {
    const float brightness = node < glow_brightness_.size() ? glow_brightness_[node] : 0.0F;
    draw_glow(draw, frame_min, ui::progress_frame_for_icon(icon_size), brightness,
              static_cast<float>(node) * 2.39996323F);
}

void WindowRenderer::Impl::render_progress_view(const std::shared_ptr<const PublishedState>& state,
                                                const std::optional<Error>& runtime_error, const float scroll_speed,
                                                const bool scroll_right) {
    control_message_.clear();
    footer_message_.clear();
    footer_color_ = {1.0F, 0.65F, 0.25F, 1.0F};
    if (runtime_error) {
        footer_message_ = runtime_error->message + ": " + runtime_error->context;
    }
    if (!state) {
        const auto origin = ImGui::GetCursorScreenPos();
        const auto available = ImGui::GetContentRegionAvail();
        if (!overlay_) {
            if (footer_message_.empty()) {
                control_message_ = "未找到 Minecraft 存档";
            }
            const float status_height = status_footer_height();
            const float controls_height = from_icon(40.0F);
            const float footer_top = origin.y + available.y - status_height - controls_height;
            (void)draw_main_controls(origin, footer_top, status_height);
            draw_status_footer(origin, available, footer_top);
        } else if (!footer_message_.empty()) {
            draw_status_footer(origin, available, origin.y + available.y - status_footer_height());
        }
        return;
    }
    if (footer_message_.empty() && state->error && state->error->code != ErrorCode::NoSave) {
        footer_message_ = state->error->message + ": " + state->error->context;
        footer_color_ = {1.0F, 0.35F, 0.25F, 1.0F};
    }
    if (footer_message_.empty() && state->data_status == DataStatus::Stale) {
        footer_message_ = "进度已过期，正在显示上次成功读取的数据";
    } else if (footer_message_.empty() && !state->has_data() && !overlay_) {
        control_message_ = "未找到 Minecraft 存档";
    }
    if (footer_message_.empty() && !state->warnings.empty()) {
        const auto& warning = state->warnings.front();
        footer_message_ = "存档扫描警告 (" + std::to_string(state->warnings.size()) + "): " + warning.message + ": " +
                          warning.context;
    }
    update_progress_glows(*state);
    if (overlay_) {
        if (completion_view_) {
            draw_completion_view(*state);
            const auto origin = ImGui::GetCursorScreenPos();
            const auto available = ImGui::GetContentRegionAvail();
            draw_status_footer(origin, available, origin.y + available.y - status_footer_height());
            return;
        }
        draw_header(*state);
    }
    draw_responsive_layout(*state, scroll_speed, scroll_right);
}

float WindowRenderer::Impl::status_footer_height() const {
    return footer_message_.empty() ? 0.0F : ImGui::GetTextLineHeight() + from_icon(8.0F);
}

float WindowRenderer::Impl::draw_main_controls(const ImVec2 origin, const float footer_top, const float status_height) {
    const float controls_height = from_icon(40.0F);
    const float footer_center_y = footer_top + status_height + (controls_height * 0.5F);
    const float settings_button_height = ImGui::GetFrameHeight() + from_icon(8.0F);
    ImGui::SetCursorScreenPos({origin.x + from_icon(8.0F), footer_center_y - (settings_button_height * 0.5F)});
    if (textured_button(assets_->widget("button.png"), assets_->widget("button_highlighted.png"), "##settings", "设置",
                        {from_icon(96.0F), settings_button_height})) {
        settings_panel_.open();
    }
    if (manual_runtime_ != nullptr) {
        ImGui::SameLine();
        if (textured_button(assets_->widget("button.png"), assets_->widget("button_highlighted.png"), "##restart-run",
                            "重开本轮", {from_icon(96.0F), settings_button_height}))
            (void)manual_runtime_->restart_run();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("清除本轮手动标记并重新读取存档，不修改 Minecraft 存档");
    }
    if (!control_message_.empty()) {
        ImGui::SameLine(0.0F, from_icon(8.0F));
        const auto text_x = ImGui::GetCursorScreenPos().x;
        ImGui::SetCursorScreenPos({text_x, footer_center_y - (ImGui::GetTextLineHeight() * 0.5F)});
        ImGui::PushStyleColor(ImGuiCol_Text, footer_color_);
        ImGui::TextUnformatted(control_message_.c_str());
        ImGui::PopStyleColor();
    }
    return ImGui::GetItemRectMax().x + from_icon(8.0F);
}

void WindowRenderer::Impl::draw_status_footer(const ImVec2 origin, const ImVec2 available,
                                              const float footer_top) const {
    if (footer_message_.empty()) {
        return;
    }
    const float height = status_footer_height();
    auto* const draw = ImGui::GetWindowDrawList();
    draw->PushClipRect({origin.x, footer_top}, {origin.x + available.x, footer_top + height}, true);
    draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                  {origin.x + from_icon(8.0F), footer_top + ((height - ImGui::GetTextLineHeight()) * 0.5F)},
                  ImGui::GetColorU32(footer_color_), footer_message_.c_str());
    draw->PopClipRect();
}

void WindowRenderer::Impl::set_completion_view(const bool complete) {
    if (complete == completion_view_) {
        return;
    }
    completion_view_ = complete;
    fireworks_.reset();
    if (complete) {
        SDL_SetWindowMinimumSize(context_.window(), 1, 1);
    } else {
        SDL_SetWindowMinimumSize(context_.window(), 1, overlay_min_height(0.0F, scale_));
    }
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
    draw->AddText(font, completion_font_size, {text_x, title_y}, IM_COL32(255, 215, 0, 255), template_name.c_str());
    draw->AddText(font, completion_font_size, {text_x, igt_y}, IM_COL32(255, 215, 0, 255), igt.c_str());
}

void WindowRenderer::Impl::draw_player_card(const PublishedState& state, const float center_y) const {
    const float avatar_size = icon_size();
    if (avatar_texture_ != nullptr) {
        if (is_complete(state)) {
            draw_glow(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), avatar_size, 1.0F, 0.0F, 2.1F);
        }
        ImGui::Image(avatar_texture_, {avatar_size, avatar_size});
        ImGui::SameLine();
    }
    const auto text_pos = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos({text_pos.x, center_y - (ImGui::GetTextLineHeight() * 0.5F)});
    ImGui::TextUnformatted(player_card_ ? player_card_->name.c_str() : "Steve");
}

void WindowRenderer::Impl::draw_completion_progress(const ProgressStats& stats, const std::string& progress_text,
                                                    const std::string& percentage_text, const float center_y) const {
    const float ratio = stats.ratio();
    const float bar_width = from_icon(256.0F);
    const float bar_height = from_icon(8.0F);
    const float spacing = bar_height;

    ImGui::SameLine(0.0F, from_icon(12.0F));
    const auto position = ImGui::GetCursorScreenPos();
    const auto progress_size = ImGui::CalcTextSize(progress_text.c_str());
    const auto percentage_size = ImGui::CalcTextSize(percentage_text.c_str());
    const float bar_x = position.x + progress_size.x + spacing;
    const float bar_y = center_y - (bar_height * 0.5F);
    auto* const draw = ImGui::GetWindowDrawList();
    const auto text_color = ImGui::GetColorU32(ImGuiCol_Text);
    draw->AddText({position.x, center_y - (progress_size.y * 0.5F)}, text_color, progress_text.c_str());
    if (auto* const background = assets_->ui("progress/background.png"); background != nullptr) {
        draw->AddImage(background, {bar_x, bar_y}, {bar_x + bar_width, bar_y + bar_height});
    }
    if (ratio > 0.0F) {
        if (auto* const progress = assets_->ui("progress/progress.png"); progress != nullptr) {
            draw->AddImage(progress, {bar_x, bar_y}, {bar_x + (bar_width * ratio), bar_y + bar_height}, {0, 0},
                           {ratio, 1});
        }
    }
    draw->AddText({bar_x + bar_width + spacing, center_y - (percentage_size.y * 0.5F)}, text_color,
                  percentage_text.c_str());
    ImGui::Dummy({progress_size.x + (spacing * 2.0F) + bar_width + percentage_size.x, ImGui::GetTextLineHeight()});
}

void WindowRenderer::Impl::draw_header(const PublishedState& state) {
#ifdef BEACON_VERSION
    constexpr auto beacon_version = BEACON_VERSION;
#else
    constexpr auto beacon_version = "dev";
#endif
    const auto origin = ImGui::GetCursorScreenPos();
    const float center_y = origin.y + from_icon(16.0F);
    const float text_y = center_y - (ImGui::GetTextLineHeight() * 0.5F);
    const auto beacon = std::string("Beacon ") + beacon_version;
    const auto nickname = player_card_ ? player_card_->name : std::string("Steve");
    const auto template_name = std::string(state.localization->text(state.compiled->template_name_key));
    const auto igt = std::string("IGT: ") + format_igt(state.snapshot.play_ticks);
    const bool has_refreshed = state.last_successful_read_at != 0;
    const auto refreshed = has_refreshed ? std::string("读取于 ") +
                                               format_elapsed(std::chrono::duration_cast<std::chrono::seconds>(
                                                                  std::chrono::system_clock::now().time_since_epoch())
                                                                  .count() -
                                                              state.last_successful_read_at) +
                                               " 前"
                                         : std::string{};
    const auto stats = summarize_progress(state, view_.goal_nodes);
    const auto progress = std::to_string(stats.completed) + " / " + std::to_string(stats.total);
    const auto percentage = format_percentage(stats.ratio());
    constexpr float igt_scale = 1.5F;
    const float igt_font_size = ImGui::GetFontSize() * igt_scale;
    const float avatar_size = icon_size();
    const float progress_bar_width = from_icon(256.0F);
    const float spacing = from_icon(8.0F);
    const float separator_spacing = from_icon(12.0F);
    const auto separator_size = ImGui::CalcTextSize("|");
    const auto player_size = ImGui::CalcTextSize(nickname.c_str());
    const auto igt_size =
        ImGui::GetFont()->CalcTextSizeA(igt_font_size, std::numeric_limits<float>::max(), 0.0F, igt.c_str());
    const auto progress_size = ImGui::CalcTextSize(progress.c_str());
    const auto percentage_size = ImGui::CalcTextSize(percentage.c_str());
    const float player_width =
        player_size.x + (avatar_texture_ != nullptr ? avatar_size + ImGui::GetStyle().ItemSpacing.x : 0.0F);
    const float completion_width = progress_size.x + (spacing * 2.0F) + progress_bar_width + percentage_size.x;
    const float header_width =
        ImGui::CalcTextSize(beacon.c_str()).x + player_width + ImGui::CalcTextSize(template_name.c_str()).x +
        ImGui::CalcTextSize(refreshed.c_str()).x + igt_size.x + completion_width +
        (separator_size.x + (separator_spacing * 2.0F)) * static_cast<float>(has_refreshed ? 5 : 4);
    ImGui::SetCursorPosX(overlay_ ? (ImGui::GetWindowWidth() - header_width) * 0.5F
                                  : ImGui::GetWindowWidth() - header_width - from_icon(8.0F));
    ImGui::SetCursorScreenPos({ImGui::GetCursorScreenPos().x, text_y});

    const auto draw_text = [&](const std::string& value) {
        ImGui::SetCursorScreenPos({ImGui::GetCursorScreenPos().x, text_y});
        ImGui::TextUnformatted(value.c_str());
    };
    const auto draw_separator = [&] {
        ImGui::SameLine(0.0F, separator_spacing);
        ImGui::TextUnformatted("|");
        ImGui::SameLine(0.0F, separator_spacing);
    };
    const auto draw_igt = [&] {
        const auto cursor = ImGui::GetCursorScreenPos();
        auto* const font = ImGui::GetFont();
        // Match visible digit centers: line boxes include unequal padding at different font sizes.
        const auto digit_center = [&](const float size) {
            auto* const baked = font->GetFontBaked(size);
            const auto* const glyph = baked->FindGlyph('0');
            return (glyph->Y0 + glyph->Y1) * 0.5F * (size / baked->Size);
        };
        const float base_center = std::trunc(text_y) + digit_center(ImGui::GetFontSize());
        const float igt_y = std::round(base_center - digit_center(igt_font_size));
        ImGui::GetWindowDrawList()->AddText(font, igt_font_size, {cursor.x, igt_y},
                                            ImGui::GetColorU32(ImVec4{1.0F, 215.0F / 255.0F, 0.0F, 1.0F}), igt.c_str());
        ImGui::Dummy({igt_size.x, ImGui::GetTextLineHeight()});
    };

    draw_text(beacon);
    draw_separator();
    draw_text(template_name);
    draw_separator();
    ImGui::SetCursorScreenPos({ImGui::GetCursorScreenPos().x, center_y - from_icon(16.0F)});
    draw_player_card(state, center_y);
    if (has_refreshed) {
        draw_separator();
        draw_text(refreshed);
    }
    draw_separator();
    draw_igt();
    draw_separator();
    draw_completion_progress(stats, progress, percentage, center_y);
    ImGui::SetCursorScreenPos({origin.x, origin.y + from_icon(32.0F)});
    ImGui::Dummy({0.0F, 0.0F});
}

void WindowRenderer::Impl::draw_node_icon(const PublishedState& state, ImDrawList* draw, const std::uint32_t node,
                                          const ImVec2 frame_position, const float frame_size,
                                          const ImVec2 icon_position, const float icon_size, const bool show_frame,
                                          const bool draw_glow, const ImU32 icon_tint,
                                          ImDrawListSplitter* splitter) const {
    if (node >= state.snapshot.results.size() || node >= state.compiled->presentation_by_node.size()) {
        return;
    }
    const auto& presentation = state.compiled->presentation_by_node[node];
    if (!presentation) {
        return;
    }
    const auto& result = state.snapshot.results[node];
    const auto draw_atlas_image = [&](const std::filesystem::path& path, const ImVec2 position, const float size,
                                      const ImU32 tint = IM_COL32(255, 255, 255, 255)) {
        const auto clip_min = draw->GetClipRectMin();
        const auto clip_max = draw->GetClipRectMax();
        if (position.x + size < clip_min.x || position.x > clip_max.x || position.y + size < clip_min.y ||
            position.y > clip_max.y)
            return;
        if (path.extension() == ".gif" || path.extension() == ".GIF") {
            if (auto* const frame = assets_->animated_frame(path, SDL_GetTicks())) {
                draw->AddImage(frame, position, {position.x + size, position.y + size}, {0, 0}, {1, 1}, tint);
            }
            return;
        }
        const auto found = atlas_regions_.find(path);
        if (found == atlas_regions_.end()) {
            if (auto* const texture = assets_->cached(path)) {
                draw->AddImage(texture, position, {position.x + size, position.y + size}, {0, 0}, {1, 1}, tint);
            }
            return;
        }
        draw->AddImage(atlas_texture_.get(), position, {position.x + size, position.y + size}, found->second.uv_min,
                       found->second.uv_max, tint);
    };
    if (show_frame) {
        if (splitter)
            splitter->SetCurrentChannel(draw, 0);
        draw_atlas_image(
            path_from_utf8(result.done ? presentation->frame_obtained_path : presentation->frame_unobtained_path),
            frame_position, frame_size);
    }
    if (draw_glow) {
        if (splitter)
            splitter->SetCurrentChannel(draw, 1);
        draw_progress_glow(draw, node, frame_position, icon_size);
    }
    if (splitter)
        splitter->SetCurrentChannel(draw, 2);
    draw_atlas_image(path_from_utf8(presentation->icon_path), icon_position, icon_size, icon_tint);
}

void WindowRenderer::Impl::draw_item(const PublishedState& state, const LayoutGroup& group, ImDrawList* draw,
                                     const std::uint32_t index, const ItemLayout& item, ImDrawListSplitter& splitter) {
    if (index >= state.snapshot.results.size() || index >= view_.labels.size()) {
        return;
    }
    const auto& result = state.snapshot.results[index];
    const auto hit_min = item.show_frame ? ImVec2{item.frame_x, item.frame_y} : ImVec2{item.icon_x, item.icon_y};
    const auto hit_size = item.show_frame ? item.frame_size : item.icon_size;
    if (!overlay_ && manual_runtime_ != nullptr && ImGui::GetIO().KeyCtrl &&
        ImGui::IsMouseHoveringRect(hit_min, {hit_min.x + hit_size, hit_min.y + hit_size})) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            (void)manual_runtime_->manual_operation(index, true);
        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            (void)manual_runtime_->manual_operation(index, false);
        }
    }
    const auto foreground =
        item.collection_item && !result.done ? IM_COL32(95, 95, 100, 255) : IM_COL32(245, 245, 245, 255);
    draw_node_icon(state, draw, index, {item.frame_x, item.frame_y}, item.frame_size, {item.icon_x, item.icon_y},
                   item.icon_size, item.show_frame, item.draw_glow,
                   item.collection_item && !result.done ? IM_COL32(70, 70, 70, 255) : IM_COL32(255, 255, 255, 255),
                   &splitter);
    splitter.SetCurrentChannel(draw, 3);

    const auto& label = view_.labels[index];
    if (group.source != LayoutSource::Children) {
        const float label_y = overlay_ ? item.cell_y + item.cell_height + from_icon(2.0F)
                                       : item.cell_y + item.frame_size + from_icon(4.0F);
        const float title_wrap_width = item.frame_size + item.icon_size;
        auto line_count = draw_progress_text(draw, label, item.cell_x + (item.cell_width * 0.5F), label_y, foreground,
                                             title_wrap_width);
        if (item.stats) {
            const auto progress = format_rule_progress(result);
            draw_progress_text(draw, progress, item.cell_x + (item.cell_width * 0.5F),
                               label_y + (ImGui::GetFontSize() * line_count), foreground);
            ++line_count;
        }
        if (item.scrollable && result.done) {
            const float cover_width = item.frame_size + item.icon_size;
            completion_.draw_cover(
                draw, index, {item.cell_x + ((item.cell_width - cover_width) * 0.5F), item.cell_y},
                {cover_width, item.cell_height + from_icon(6.0F) + (ImGui::GetFontSize() * line_count)});
        }
    } else if (item.collection_item) {
        const auto label_y = item.cell_y + ((item.frame_size - ImGui::GetFontSize()) * 0.5F);
        const auto label_x = item.cell_x + (item.icon_size * 1.5F) + from_icon(6.0F);
        draw_progress_text(draw, label, label_x + (ImGui::CalcTextSize(label.c_str()).x * 0.5F), label_y, foreground);
    } else if (item.scrollable && result.done) {
        completion_.draw_cover(draw, index, {item.icon_x, item.icon_y}, {item.icon_size, item.icon_size});
    }
}

}  // namespace beacon
