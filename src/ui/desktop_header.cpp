#include "desktop_renderer_internal.hpp"

#include <beacon/ui/format.hpp>

#include <chrono>
#include <cmath>
#include <limits>
#include <string>

namespace beacon {
namespace {

constexpr float header_igt_scale = 1.5F;
// §7 gray for separators and secondary details.
constexpr ImVec4 secondary_text_color{170.0F / 255.0F, 170.0F / 255.0F, 170.0F / 255.0F, 1.0F};

}  // namespace

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
    draw_nine_slice(draw, assets_->image("progress/background.png"), {bar_x, bar_y},
                    {bar_x + bar_width, bar_y + bar_height}, frame_slice, bar_unit);
    if (ratio > 0.0F) {
        // Reveal the full-width fill up to the ratio, like the vanilla experience bar.
        draw->PushClipRect({bar_x, bar_y}, {snap_to_pixel(bar_x + (bar_width * ratio)), bar_y + bar_height}, true);
        draw_nine_slice(draw, assets_->image("progress/progress.png"), {bar_x, bar_y},
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

}  // namespace beacon
