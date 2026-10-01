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

// Minecraft chat formatting colors: §c red, §6 gold, §a green.
constexpr ImVec4 error_text_color{1.0F, 85.0F / 255.0F, 85.0F / 255.0F, 1.0F};
constexpr ImVec4 warning_text_color{1.0F, 170.0F / 255.0F, 0.0F, 1.0F};
constexpr ImU32 update_text_color = IM_COL32(85, 255, 85, 255);
constexpr ImVec4 success_text_color{85.0F / 255.0F, 1.0F, 85.0F / 255.0F, 1.0F};
constexpr float header_igt_scale = 1.5F;
// §7 gray for separators and secondary details.
constexpr ImVec4 secondary_text_color{170.0F / 255.0F, 170.0F / 255.0F, 170.0F / 255.0F, 1.0F};

std::string update_button_label(const UpdateNotice& notice, std::string_view language) {
    return std::string(interface_text(language, "更新到 ", "Update to ")) + notice.version;
}

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
    if (brightness <= 0.001F || glow_texture_ == nullptr) {
        return;
    }
    const float extent = frame_size * scale * 1.42F;
    const ImVec2 center{frame_min.x + (frame_size * 0.5F), frame_min.y + (frame_size * 0.5F)};
    const auto clip_min = draw->GetClipRectMin();
    const auto clip_max = draw->GetClipRectMax();
    if (center.x + extent < clip_min.x || center.x - extent > clip_max.x || center.y + extent < clip_min.y ||
        center.y - extent > clip_max.y)
        return;
    continuous_animation_ = true;
    const float time = glow_animation_time_;
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
    footer_message_.clear();
    footer_color_ = warning_text_color;
    if (!overlay_ && SDL_GetTicks() < feedback_until_) {
        footer_message_ = feedback_message_;
        footer_color_ = feedback_color_;
    } else if (runtime_error) {
        footer_message_ = runtime_error->message + ": " + runtime_error->context;
    } else if (update_notice_ != nullptr && !update_notice_->error.empty()) {
        footer_message_ = update_notice_->error;
        footer_color_ = error_text_color;
    }
    if (!state) {
        const auto origin = ImGui::GetCursorScreenPos();
        const auto available = ImGui::GetContentRegionAvail();
        if (!overlay_) {
            if (footer_message_.empty())
                footer_message_ = text("未找到 Minecraft 存档", "No Minecraft save found");
            draw_main_footer(nullptr, nullptr, origin, available);
        } else if (!footer_message_.empty()) {
            draw_status_footer(origin, available, origin.y + available.y - status_footer_height());
        }
        return;
    }
    if (footer_message_.empty() && state->error && state->error->code != ErrorCode::NoSave) {
        footer_message_ = state->error->message + ": " + state->error->context;
        footer_color_ = error_text_color;
    }
    if (footer_message_.empty() && state->data_status == DataStatus::Stale) {
        footer_message_ =
            text("进度已过期，正在显示上次成功读取的数据", "Progress is stale; showing the last successful read");
    } else if (footer_message_.empty() && !state->has_data() && !overlay_) {
        footer_message_ = text("未找到 Minecraft 存档", "No Minecraft save found");
    }
    if (footer_message_.empty() && !state->warnings.empty()) {
        const auto& warning = state->warnings.front();
        footer_message_ = std::string(text("存档扫描警告 (", "Save scan warnings (")) +
                          std::to_string(state->warnings.size()) + "): " + warning.message + ": " + warning.context;
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
        draw_header(*state, make_header(*state));
    }
    draw_responsive_layout(*state, scroll_speed, scroll_right);
}

void WindowRenderer::Impl::restart_run() {
    if (manual_runtime_ == nullptr)
        return;
    if (const auto restarted = manual_runtime_->restart_run(); restarted) {
        feedback_message_ =
            text("已重开本轮：手动标记已清除，正在重新读取存档", "Run restarted: marks cleared; reloading save");
        feedback_color_ = success_text_color;
    } else {
        feedback_message_ = std::string(text("重开本轮失败：", "Restart failed: ")) + restarted.error().message;
        feedback_color_ = error_text_color;
    }
    feedback_until_ = SDL_GetTicks() + 2500;
    // Keep drawing until the confirmation disappears.
    interactive_until_ = std::max(interactive_until_, feedback_until_ + 50);
}

float WindowRenderer::Impl::status_footer_height() const {
    // The main window shows messages inside its control row.
    return overlay_ && !footer_message_.empty() ? ImGui::GetTextLineHeight() + from_icon(8.0F) : 0.0F;
}

float WindowRenderer::Impl::draw_main_controls(const ImVec2 origin, const float center_y) {
    const float button_height = ImGui::GetFrameHeight() + from_icon(8.0F);
    ImGui::SetCursorScreenPos({origin.x + from_icon(8.0F), center_y - (button_height * 0.5F)});
    if (minecraft_button(*assets_, "##settings", text("设置", "Settings"), {from_icon(96.0F), button_height})) {
        settings_panel_.open();
    }
    if (update_notice_ != nullptr && !update_notice_->version.empty()) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(update_text_color));
        if (minecraft_button(*assets_, "##apply-update", update_button_label(*update_notice_, ui_language_).c_str(),
                             {update_button_width(), button_height}))
            update_notice_->apply_requested = true;
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) {
            // Launch failures are reported in the status line, so the tooltip only explains the action.
            const auto tooltip =
                std::string(text("发现新版本 ", "New version available: ")) + update_notice_->version +
                text("\n点击后将关闭 Beacon 并安装更新", "\nClick to close Beacon and install the update");
            minecraft_tooltip(tooltip.c_str());
        }
    }
    return ImGui::GetItemRectMax().x + from_icon(16.0F);
}

void WindowRenderer::Impl::draw_main_footer(const PublishedState* state, const HeaderText* header, const ImVec2 origin,
                                            const ImVec2 available) {
    const float controls_height = from_icon(40.0F);
    const float footer_top = origin.y + available.y - controls_height;
    const float center_y = footer_top + (controls_height * 0.5F);
    const float left = draw_main_controls(origin, center_y);
    const float right = origin.x + available.x - from_icon(8.0F);
    const float row_width = std::max(0.0F, right - left);
    const float gap = from_icon(16.0F);

    // Buttons, message and header share one row. When it is crowded the header first drops its
    // least useful segments, then the message is shortened; IGT and completion keep priority.
    std::optional<HeaderText> fitted;
    if (header != nullptr)
        fitted = *header;
    const float header_minimum = fitted ? fitted->minimum_width() + gap : 0.0F;
    float header_left = left;
    if (!footer_message_.empty() && row_width > 0.0F) {
        const float full_width = ImGui::CalcTextSize(footer_message_.c_str()).x;
        const float message_width =
            std::min(row_width, full_width + header_minimum <= row_width
                                    ? full_width
                                    : std::max(row_width - header_minimum, std::min(full_width, from_icon(96.0F))));
        const auto shown = ellipsize_text(footer_message_, message_width);
        const ImVec2 position{left, center_y - (ImGui::GetTextLineHeight() * 0.5F)};
        auto* const draw = ImGui::GetWindowDrawList();
        draw->PushClipRect({left, footer_top}, {left + message_width + from_icon(2.0F), footer_top + controls_height},
                           true);
        draw_shadowed_text(draw, position, ImGui::GetColorU32(footer_color_), shown.c_str());
        draw->PopClipRect();
        if (shown.size() != footer_message_.size() &&
            ImGui::IsMouseHoveringRect(position, {left + message_width, position.y + ImGui::GetTextLineHeight()}))
            minecraft_tooltip(footer_message_.c_str());
        header_left = left + message_width + gap;
    }
    if (fitted)
        fitted->fit(right - header_left);
    if (state != nullptr && fitted) {
        // The clip allows for truncated glyph origins on the left and the text shadow on the right.
        ImGui::PushClipRect({header_left - from_icon(4.0F), footer_top},
                            {right + from_icon(2.0F), footer_top + controls_height}, true);
        ImGui::SetCursorScreenPos({origin.x, center_y - from_icon(16.0F)});
        draw_header(*state, *fitted, right);
        ImGui::PopClipRect();
    }
}

void WindowRenderer::Impl::draw_status_footer(const ImVec2 origin, const ImVec2 available,
                                              const float footer_top) const {
    if (footer_message_.empty()) {
        return;
    }
    const float height = status_footer_height();
    auto* const draw = ImGui::GetWindowDrawList();
    draw->PushClipRect({origin.x, footer_top}, {origin.x + available.x, footer_top + height}, true);
    draw_shadowed_text(draw, {origin.x + from_icon(8.0F), footer_top + ((height - ImGui::GetTextLineHeight()) * 0.5F)},
                       ImGui::GetColorU32(footer_color_), footer_message_.c_str());
    draw->PopClipRect();
}

float WindowRenderer::Impl::update_button_width() const {
    if (overlay_ || update_notice_ == nullptr || update_notice_->version.empty())
        return 0.0F;
    return std::max(from_icon(96.0F),
                    ImGui::CalcTextSize(update_button_label(*update_notice_, ui_language_).c_str()).x +
                        from_icon(24.0F));
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
    draw_shadowed_text(draw, font, completion_font_size, {text_x, title_y}, IM_COL32(255, 215, 0, 255),
                       template_name.c_str());
    draw_shadowed_text(draw, font, completion_font_size, {text_x, igt_y}, IM_COL32(255, 215, 0, 255), igt.c_str());
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
    shadowed_text(player_card_ ? player_card_->name.c_str() : "Steve");
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
    draw_shadowed_text(draw, {position.x, center_y - (progress_size.y * 0.5F)}, text_color, progress_text.c_str());
    // Experience-bar textures are 5 texels tall; keep their 1-texel caps crisp at any width.
    const float bar_unit = bar_height / 5.0F;
    draw_nine_slice(draw, assets_->ui("progress/background.png"), {bar_x, bar_y},
                    {bar_x + bar_width, bar_y + bar_height}, frame_slice, bar_unit);
    if (ratio > 0.0F) {
        // Reveal the full-width fill up to the ratio, like the vanilla experience bar.
        draw->PushClipRect({bar_x, bar_y}, {snap_to_pixel(bar_x + (bar_width * ratio)), bar_y + bar_height}, true);
        draw_nine_slice(draw, assets_->ui("progress/progress.png"), {bar_x, bar_y},
                        {bar_x + bar_width, bar_y + bar_height}, frame_slice, bar_unit);
        draw->PopClipRect();
    }
    draw_shadowed_text(draw, {bar_x + bar_width + spacing, center_y - (percentage_size.y * 0.5F)}, text_color,
                       percentage_text.c_str());
    ImGui::Dummy({progress_size.x + (spacing * 2.0F) + bar_width + percentage_size.x, ImGui::GetTextLineHeight()});
}

WindowRenderer::Impl::HeaderText WindowRenderer::Impl::make_header(const PublishedState& state) const {
#ifdef BEACON_VERSION
    constexpr auto beacon_version = BEACON_VERSION;
#else
    constexpr auto beacon_version = "dev";
#endif
    HeaderText header;
    header.beacon = std::string("Beacon ") + beacon_version;
    header.nickname = player_card_ ? player_card_->name : std::string("Steve");
    header.template_name = std::string(state.localization->text(state.compiled->template_name_key));
    header.igt = std::string("IGT: ") + format_igt(state.snapshot.play_ticks);
    if (state.last_successful_read_at != 0) {
        const auto now =
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                .count();
        header.refreshed = std::string(text("读取于 ", "Read ")) + format_elapsed(now - state.last_successful_read_at) +
                           text(" 前", " ago");
    }
    header.stats = summarize_progress(state, view_.goal_nodes);
    header.progress = std::to_string(header.stats.completed) + " / " + std::to_string(header.stats.total);
    header.percentage = format_percentage(header.stats.ratio());

    const float spacing = from_icon(8.0F);
    const float separator_spacing = from_icon(12.0F);
    auto& widths = header.segment_widths;
    widths[HeaderText::beacon_segment] = ImGui::CalcTextSize(header.beacon.c_str()).x;
    widths[HeaderText::template_segment] = ImGui::CalcTextSize(header.template_name.c_str()).x;
    widths[HeaderText::player_segment] =
        ImGui::CalcTextSize(header.nickname.c_str()).x +
        (avatar_texture_ != nullptr ? icon_size() + ImGui::GetStyle().ItemSpacing.x : 0.0F);
    widths[HeaderText::refreshed_segment] = ImGui::CalcTextSize(header.refreshed.c_str()).x;
    widths[HeaderText::igt_segment] = ImGui::GetFont()
                                          ->CalcTextSizeA(ImGui::GetFontSize() * header_igt_scale,
                                                          std::numeric_limits<float>::max(), 0.0F, header.igt.c_str())
                                          .x;
    widths[HeaderText::completion_segment] = ImGui::CalcTextSize(header.progress.c_str()).x + (spacing * 2.0F) +
                                             from_icon(256.0F) + ImGui::CalcTextSize(header.percentage.c_str()).x;
    header.separator_width = ImGui::CalcTextSize("|").x + (separator_spacing * 2.0F);
    header.visible.fill(true);
    header.visible[HeaderText::refreshed_segment] = !header.refreshed.empty();
    return header;
}

float WindowRenderer::Impl::HeaderText::width() const {
    float total = 0.0F;
    std::size_t count = 0;
    for (std::size_t segment = 0; segment < segment_count; ++segment) {
        if (visible[segment]) {
            total += segment_widths[segment];
            ++count;
        }
    }
    return total + (separator_width * static_cast<float>(count > 0 ? count - 1 : 0));
}

void WindowRenderer::Impl::HeaderText::fit(const float max_width) {
    // Least useful mid-run first; IGT and completion always stay.
    for (const auto segment : {beacon_segment, refreshed_segment, template_segment, player_segment}) {
        if (width() <= max_width)
            return;
        visible[segment] = false;
    }
}

float WindowRenderer::Impl::HeaderText::minimum_width() const {
    auto minimum = *this;
    minimum.fit(0.0F);
    return minimum.width();
}

void WindowRenderer::Impl::draw_header(const PublishedState& state, const HeaderText& header, const float right_edge) {
    const auto origin = ImGui::GetCursorScreenPos();
    const float center_y = origin.y + from_icon(16.0F);
    const float text_y = center_y - (ImGui::GetTextLineHeight() * 0.5F);
    const float igt_font_size = ImGui::GetFontSize() * header_igt_scale;
    const float separator_spacing = from_icon(12.0F);
    const float header_width = header.width();
    const float header_x = overlay_ ? ImGui::GetWindowPos().x + ((ImGui::GetWindowWidth() - header_width) * 0.5F)
                                    : right_edge - header_width;
    ImGui::SetCursorScreenPos({header_x, text_y});

    const auto draw_text = [&](const std::string& value, const ImVec4& color = {1.0F, 1.0F, 1.0F, 1.0F}) {
        ImGui::SetCursorScreenPos({ImGui::GetCursorScreenPos().x, text_y});
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        shadowed_text(value.c_str());
        ImGui::PopStyleColor();
    };
    const auto draw_separator = [&] {
        ImGui::SameLine(0.0F, separator_spacing);
        draw_text("|", secondary_text_color);
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
        draw_shadowed_text(ImGui::GetWindowDrawList(), font, igt_font_size, {cursor.x, igt_y},
                           IM_COL32(255, 215, 0, 255), header.igt.c_str());
        ImGui::Dummy({font->CalcTextSizeA(igt_font_size, std::numeric_limits<float>::max(), 0.0F, header.igt.c_str()).x,
                      ImGui::GetTextLineHeight()});
    };

    bool first = true;
    for (std::size_t segment = 0; segment < HeaderText::segment_count; ++segment) {
        if (!header.visible[segment])
            continue;
        if (!first)
            draw_separator();
        first = false;
        switch (segment) {
            case HeaderText::beacon_segment:
                draw_text(header.beacon);
                break;
            case HeaderText::template_segment:
                draw_text(header.template_name);
                break;
            case HeaderText::player_segment:
                ImGui::SetCursorScreenPos({ImGui::GetCursorScreenPos().x, center_y - from_icon(16.0F)});
                draw_player_card(state, center_y);
                break;
            case HeaderText::refreshed_segment:
                draw_text(header.refreshed, secondary_text_color);
                break;
            case HeaderText::igt_segment:
                draw_igt();
                break;
            default:
                draw_completion_progress(header.stats, header.progress, header.percentage, center_y);
                break;
        }
    }
    ImGui::SetCursorScreenPos({origin.x, origin.y + from_icon(32.0F)});
    ImGui::Dummy({0.0F, 0.0F});
}

void WindowRenderer::Impl::draw_node_icon(const PublishedState& state, ImDrawList* draw, const std::uint32_t node,
                                          const ImVec2 frame_position, const float frame_size,
                                          const ImVec2 icon_position, const float icon_size, const bool show_frame,
                                          const bool draw_glow, const ImU32 icon_tint,
                                          ImDrawListSplitter* splitter) const {
    if (node >= state.snapshot.results.size() || node >= node_sprites_.size() ||
        node >= state.compiled->presentation_by_node.size() || !state.compiled->presentation_by_node[node]) {
        return;
    }
    const auto& sprites = node_sprites_[node];
    const auto draw_sprite = [&](const Sprite& sprite, const ImVec2 position, const float size,
                                 const ImU32 tint = IM_COL32(255, 255, 255, 255)) {
        const auto clip_min = draw->GetClipRectMin();
        const auto clip_max = draw->GetClipRectMax();
        if (position.x + size < clip_min.x || position.x > clip_max.x || position.y + size < clip_min.y ||
            position.y > clip_max.y)
            return;
        auto* const texture =
            sprite.animation.empty() ? sprite.texture : assets_->animated_frame(sprite.animation, SDL_GetTicks());
        if (texture != nullptr)
            draw->AddImage(texture, position, {position.x + size, position.y + size}, sprite.uv_min, sprite.uv_max,
                           tint);
    };
    if (show_frame) {
        if (splitter)
            splitter->SetCurrentChannel(draw, 0);
        draw_sprite(state.snapshot.results[node].done ? sprites.frame_obtained : sprites.frame_unobtained,
                    frame_position, frame_size);
    }
    if (draw_glow) {
        if (splitter)
            splitter->SetCurrentChannel(draw, 1);
        draw_progress_glow(draw, node, frame_position, icon_size);
    }
    if (splitter)
        splitter->SetCurrentChannel(draw, 2);
    draw_sprite(sprites.icon, icon_position, icon_size, icon_tint);
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
    // Pending collection entries stay legible but recede: gray text, icon at roughly 40% brightness.
    const bool pending_collection = item.collection_item && !result.done;
    const auto foreground = pending_collection ? IM_COL32(128, 128, 128, 255) : IM_COL32(245, 245, 245, 255);
    draw_node_icon(state, draw, index, {item.frame_x, item.frame_y}, item.frame_size, {item.icon_x, item.icon_y},
                   item.icon_size, item.show_frame, item.draw_glow,
                   pending_collection ? IM_COL32(100, 100, 100, 255) : IM_COL32(255, 255, 255, 255), &splitter);
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
