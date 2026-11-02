#pragma once

namespace beacon::ui {

constexpr float source_icon_size = 32.0F;
constexpr float source_font_size = 12.0F;
constexpr float progress_frame_ratio = 26.0F / 16.0F;

constexpr float from_icon(const float original_size, const float scale) {
    return original_size * scale;
}

constexpr float icon_size(const float scale) {
    return from_icon(source_icon_size, scale);
}

constexpr float font_size(const float scale) {
    return from_icon(source_font_size, scale);
}

constexpr float progress_frame_size(const float scale) {
    return icon_size(scale) * progress_frame_ratio;
}

constexpr float progress_frame_for_icon(const float icon_size) {
    return icon_size * progress_frame_ratio;
}

constexpr float settings_font_size = 16.0F;

}  // namespace beacon::ui
