#include "desktop_renderer_internal.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>

namespace beacon {
namespace {

// §a green, like the vanilla "new version" notice.
constexpr ImU32 update_text_color = IM_COL32(85, 255, 85, 255);

std::string update_button_label(const UpdateNotice& notice, std::string_view language) {
    return std::string(interface_text(language, "更新到 ", "Update to ")) + notice.version;
}

}  // namespace

void WindowRenderer::Impl::restart_run() {
    if (manual_runtime_ == nullptr)
        return;
    if (const auto restarted = manual_runtime_->restart_run(); restarted) {
        status_.feedback =
            text("已重开本轮：手动标记已清除，正在重新读取存档", "Run restarted: marks cleared; reloading save");
        status_.feedback_color = success_text_color;
    } else {
        status_.feedback = std::string(text("重开本轮失败：", "Restart failed: ")) + restarted.error().message;
        status_.feedback_color = error_text_color;
    }
    status_.feedback_until = SDL_GetTicks() + 2500;
    // Keep drawing until the confirmation disappears.
    interactive_until_ = std::max(interactive_until_, status_.feedback_until + 50);
}

float WindowRenderer::Impl::status_footer_height() const {
    // The main window shows messages inside its control row.
    return overlay_ && !status_.message.empty() ? ImGui::GetTextLineHeight() + from_icon(8.0F) : 0.0F;
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

void WindowRenderer::Impl::draw_main_footer(const PublishedState* state, const ImVec2 origin, const ImVec2 available) {
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
    if (state != nullptr)
        fitted = make_header(*state);
    const float header_minimum = fitted ? fitted->minimum_width() + gap : 0.0F;
    float header_left = left;
    if (!status_.message.empty() && row_width > 0.0F) {
        const float full_width = ImGui::CalcTextSize(status_.message.c_str()).x;
        const float message_width =
            std::min(row_width, full_width + header_minimum <= row_width
                                    ? full_width
                                    : std::max(row_width - header_minimum, std::min(full_width, from_icon(96.0F))));
        const auto shown = ellipsize_text(status_.message, message_width);
        const ImVec2 position{left, center_y - (ImGui::GetTextLineHeight() * 0.5F)};
        auto* const draw = ImGui::GetWindowDrawList();
        draw->PushClipRect({left, footer_top}, {left + message_width + from_icon(2.0F), footer_top + controls_height},
                           true);
        draw_shadowed_text(draw, position, ImGui::GetColorU32(status_.color), shown.c_str());
        draw->PopClipRect();
        if (shown.size() != status_.message.size() &&
            ImGui::IsMouseHoveringRect(position, {left + message_width, position.y + ImGui::GetTextLineHeight()}))
            minecraft_tooltip(status_.message.c_str());
        header_left = left + message_width + gap;
    }
    if (fitted) {
        fitted->fit(right - header_left);
        // The clip allows for truncated glyph origins on the left and the text shadow on the right.
        ImGui::PushClipRect({header_left - from_icon(4.0F), footer_top},
                            {right + from_icon(2.0F), footer_top + controls_height}, true);
        ImGui::SetCursorScreenPos({origin.x, center_y - from_icon(16.0F)});
        draw_header(*state, *fitted, right);
        ImGui::PopClipRect();
    }
}

void WindowRenderer::Impl::draw_status_footer(const ImVec2 origin, const ImVec2 available) const {
    if (status_.message.empty()) {
        return;
    }
    const float height = status_footer_height();
    const float footer_top = origin.y + available.y - height;
    auto* const draw = ImGui::GetWindowDrawList();
    draw->PushClipRect({origin.x, footer_top}, {origin.x + available.x, footer_top + height}, true);
    draw_shadowed_text(draw, {origin.x + from_icon(8.0F), footer_top + ((height - ImGui::GetTextLineHeight()) * 0.5F)},
                       ImGui::GetColorU32(status_.color), status_.message.c_str());
    draw->PopClipRect();
}

float WindowRenderer::Impl::update_button_width() const {
    if (overlay_ || update_notice_ == nullptr || update_notice_->version.empty())
        return 0.0F;
    return std::max(from_icon(96.0F),
                    ImGui::CalcTextSize(update_button_label(*update_notice_, ui_language_).c_str()).x +
                        from_icon(24.0F));
}

}  // namespace beacon
