#include <beacon/ui/widgets.hpp>
#include <beacon/io/file.hpp>
#include <beacon/ui/sizing.hpp>

#include <SDL3_image/SDL_image.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <string_view>
#include <utility>

namespace beacon {

namespace {

void add_exact_text(ImDrawList* draw, ImFont* font, const float font_size, const ImVec2 position, const ImU32 color,
                    const char* begin, const char* end, const float wrap_width = 0.0F) {
    const int first_vertex = draw->VtxBuffer.Size;
    draw->AddText(font, font_size, position, color, begin, end, wrap_width);
    // ImFont truncates the text origin. Restore its fraction so labels move with images.
    const ImVec2 offset{position.x - std::trunc(position.x), position.y - std::trunc(position.y)};
    for (int index = first_vertex; index < draw->VtxBuffer.Size; ++index) {
        draw->VtxBuffer[index].pos.x += offset.x;
        draw->VtxBuffer[index].pos.y += offset.y;
    }
}

}  // namespace

float text_shadow_offset(ImFont* font, const float font_size) {
    // Minecraft digits are seven font pixels tall; derive the pixel from the baked glyph.
    float pixel = font_size / 12.0F;
    if (auto* const baked = font->GetFontBaked(font_size); baked != nullptr) {
        if (const auto* const glyph = baked->FindGlyphNoFallback('0'); glyph != nullptr && glyph->Y1 > glyph->Y0)
            pixel = (glyph->Y1 - glyph->Y0) * (font_size / baked->Size) / 7.0F;
    }
    // Snap to whole framebuffer pixels so the shadow stays crisp on HiDPI displays.
    const float framebuffer_scale = std::max(1.0F, ImGui::GetIO().DisplayFramebufferScale.x);
    return std::max(1.0F, std::round(pixel * framebuffer_scale)) / framebuffer_scale;
}

ImU32 text_shadow_color(const ImU32 color) {
    // Same as Minecraft's (rgb & 0xFCFCFC) >> 2, keeping the source alpha.
    return ((color & 0x00FCFCFCU) >> 2U) | (color & IM_COL32_A_MASK);
}

void draw_shadowed_text(ImDrawList* draw, ImFont* font, const float font_size, const ImVec2 position, const ImU32 color,
                        const char* begin, const char* end, const float wrap_width) {
    const float offset = text_shadow_offset(font, font_size);
    const ImVec2 origin{std::trunc(position.x), std::trunc(position.y)};
    add_exact_text(draw, font, font_size, {origin.x + offset, origin.y + offset}, text_shadow_color(color), begin, end,
                   wrap_width);
    draw->AddText(font, font_size, origin, color, begin, end, wrap_width);
}

void draw_shadowed_text(ImDrawList* draw, const ImVec2 position, const ImU32 color, const char* begin,
                        const char* end) {
    draw_shadowed_text(draw, ImGui::GetFont(), ImGui::GetFontSize(), position, color, begin, end);
}

void shadowed_text(const char* begin, const char* end) {
    const auto position = ImGui::GetCursorScreenPos();
    const auto size = ImGui::CalcTextSize(begin, end);
    draw_shadowed_text(ImGui::GetWindowDrawList(), position, ImGui::GetColorU32(ImGuiCol_Text), begin, end);
    ImGui::Dummy(size);
}

std::string ellipsize_text(const std::string& text, const float max_width) {
    if (ImGui::CalcTextSize(text.c_str()).x <= max_width)
        return text;
    constexpr std::string_view ellipsis = "...";
    const float available = max_width - ImGui::CalcTextSize(ellipsis.data()).x;
    std::size_t end = 0;
    // Advance whole UTF-8 code points while the prefix still fits.
    for (std::size_t next = 0; next < text.size();) {
        std::size_t length = 1;
        while (next + length < text.size() && (static_cast<unsigned char>(text[next + length]) & 0xC0U) == 0x80U)
            ++length;
        if (ImGui::CalcTextSize(text.data(), text.data() + next + length).x > available)
            break;
        next += length;
        end = next;
    }
    return text.substr(0, end) + std::string(ellipsis);
}

float snap_to_pixel(const float value) {
    const float scale = std::max(1.0F, ImGui::GetIO().DisplayFramebufferScale.x);
    return std::round(value * scale) / scale;
}

void draw_nine_slice(ImDrawList* draw, SDL_Texture* texture, const ImVec2 min, const ImVec2 max, const NineSlice slice,
                     const float unit, const ImU32 tint) {
    float texture_width = 0.0F;
    float texture_height = 0.0F;
    if (texture == nullptr || !SDL_GetTextureSize(texture, &texture_width, &texture_height) || texture_width <= 0.0F ||
        texture_height <= 0.0F)
        return;
    // Snap every cut to the framebuffer so all borders render with identical thickness.
    const float left = snap_to_pixel(std::min(slice.left * unit, (max.x - min.x) * 0.5F));
    const float right = snap_to_pixel(std::min(slice.right * unit, (max.x - min.x) * 0.5F));
    const float top = snap_to_pixel(std::min(slice.top * unit, (max.y - min.y) * 0.5F));
    const float bottom = snap_to_pixel(std::min(slice.bottom * unit, (max.y - min.y) * 0.5F));
    const float xs[] = {snap_to_pixel(min.x), snap_to_pixel(min.x) + left, snap_to_pixel(max.x) - right,
                        snap_to_pixel(max.x)};
    const float ys[] = {snap_to_pixel(min.y), snap_to_pixel(min.y) + top, snap_to_pixel(max.y) - bottom,
                        snap_to_pixel(max.y)};
    const float us[] = {0.0F, slice.left / texture_width, 1.0F - (slice.right / texture_width), 1.0F};
    const float vs[] = {0.0F, slice.top / texture_height, 1.0F - (slice.bottom / texture_height), 1.0F};
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            if (xs[column + 1] <= xs[column] || ys[row + 1] <= ys[row])
                continue;
            draw->AddImage(texture, {xs[column], ys[row]}, {xs[column + 1], ys[row + 1]}, {us[column], vs[row]},
                           {us[column + 1], vs[row + 1]}, tint);
        }
    }
}

void draw_inset_panel(ImDrawList* draw, const ImVec2 min, const ImVec2 max, const float unit) {
    // Colors are translucent so the panel follows the user's background color.
    const float edge = std::max(1.0F / std::max(1.0F, ImGui::GetIO().DisplayFramebufferScale.x), snap_to_pixel(unit));
    const ImVec2 a{snap_to_pixel(min.x), snap_to_pixel(min.y)};
    const ImVec2 b{snap_to_pixel(max.x), snap_to_pixel(max.y)};
    draw->AddRectFilled(a, b, IM_COL32(0, 0, 0, 48));
    draw->AddRectFilled(a, {b.x - edge, a.y + edge}, IM_COL32(0, 0, 0, 140));
    draw->AddRectFilled({a.x, a.y + edge}, {a.x + edge, b.y - edge}, IM_COL32(0, 0, 0, 140));
    draw->AddRectFilled({a.x + edge, b.y - edge}, b, IM_COL32(255, 255, 255, 40));
    draw->AddRectFilled({b.x - edge, a.y + edge}, {b.x, b.y - edge}, IM_COL32(255, 255, 255, 40));
}

void minecraft_tooltip(const char* text) {
    auto* const font = ImGui::GetFont();
    const float font_size = ImGui::GetFontSize();
    const float pixel = text_shadow_offset(font, font_size);
    const ImVec2 padding{pixel * 4.0F, pixel * 4.0F};
    const float wrap_width = std::max(font_size * 16.0F, ImGui::GetMainViewport()->WorkSize.x * 0.5F);
    const auto text_size = ImGui::CalcTextSize(text, nullptr, false, wrap_width);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 0.0F);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, {0.0F, 0.0F, 0.0F, 0.0F});
    if (ImGui::BeginTooltip()) {
        auto* const draw = ImGui::GetWindowDrawList();
        const auto min = ImGui::GetWindowPos();
        const ImVec2 max{min.x + text_size.x + (padding.x * 2.0F), min.y + text_size.y + (padding.y * 2.0F)};
        // Vanilla tooltip: near-black purple body with notched corners and a purple gradient frame.
        constexpr ImU32 background = IM_COL32(16, 0, 16, 240);
        constexpr ImU32 border_top = IM_COL32(80, 0, 255, 80);
        constexpr ImU32 border_bottom = IM_COL32(40, 0, 127, 80);
        draw->AddRectFilled({min.x + pixel, min.y}, {max.x - pixel, max.y}, background);
        draw->AddRectFilled({min.x, min.y + pixel}, {min.x + pixel, max.y - pixel}, background);
        draw->AddRectFilled({max.x - pixel, min.y + pixel}, {max.x, max.y - pixel}, background);
        const ImVec2 inner_min{min.x + pixel, min.y + pixel};
        const ImVec2 inner_max{max.x - pixel, max.y - pixel};
        draw->AddRectFilled(inner_min, {inner_max.x, inner_min.y + pixel}, border_top);
        draw->AddRectFilled({inner_min.x, inner_max.y - pixel}, inner_max, border_bottom);
        draw->AddRectFilledMultiColor({inner_min.x, inner_min.y + pixel}, {inner_min.x + pixel, inner_max.y - pixel},
                                      border_top, border_top, border_bottom, border_bottom);
        draw->AddRectFilledMultiColor({inner_max.x - pixel, inner_min.y + pixel}, {inner_max.x, inner_max.y - pixel},
                                      border_top, border_top, border_bottom, border_bottom);
        draw_shadowed_text(draw, font, font_size, {min.x + padding.x, min.y + padding.y}, IM_COL32_WHITE, text, nullptr,
                           wrap_width);
        ImGui::Dummy(text_size);
        ImGui::EndTooltip();
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
}

void draw_subpixel_text(ImDrawList* draw, const ImVec2 position, const ImU32 color, const char* begin,
                        const char* end) {
    auto* const font = ImGui::GetFont();
    const float font_size = ImGui::GetFontSize();
    const float offset = text_shadow_offset(font, font_size);
    add_exact_text(draw, font, font_size, {position.x + offset, position.y + offset}, text_shadow_color(color), begin,
                   end);
    add_exact_text(draw, font, font_size, position, color, begin, end);
}

std::size_t draw_progress_text(ImDrawList* draw, const std::string& text, const float center_x, const float y,
                               const ImU32 color, const float wrap_width) {
    std::size_t lines = 0;
    const auto draw_line = [&](const char* begin, const char* end) {
        const auto size = ImGui::CalcTextSize(begin, end);
        draw_subpixel_text(draw, {center_x - size.x * 0.5F, y + ImGui::GetFontSize() * static_cast<float>(lines)},
                           color, begin, end);
        ++lines;
    };
    const auto* begin = text.data();
    const auto* const end = begin + text.size();
    if (wrap_width <= 0.0F) {
        draw_line(begin, end);
        return lines;
    }
    while (begin < end) {
        if (*begin == '\n') {
            draw_line(begin, begin);
            ++begin;
            continue;
        }
        auto* line_end = ImGui::GetFont()->CalcWordWrapPosition(ImGui::GetFontSize(), begin, end, wrap_width);
        if (line_end <= begin) {
            line_end = begin + 1;
            while (line_end < end && (static_cast<unsigned char>(*line_end) & 0xC0) == 0x80)
                ++line_end;
        }
        draw_line(begin, line_end > begin && line_end[-1] == '\r' ? line_end - 1 : line_end);
        begin = line_end;
        while (begin < end && (*begin == ' ' || *begin == '\t'))
            ++begin;
        if (begin < end && *begin == '\n')
            ++begin;
    }
    if (text.empty() || text.back() == '\n')
        draw_line(end, end);
    return lines;
}

SDL_Texture* UiAssets::animated_frame(const std::filesystem::path& path, const std::uint64_t now_ms) {
    auto found = animations_.find(path);
    if (found == animations_.end()) {
        // Share decoded GIF frames across windows; each renderer uploads only its current frame.
        struct Decoded {
            FileStamp stamp;
            std::weak_ptr<IMG_Animation> frames;
        };
        static std::unordered_map<std::filesystem::path, Decoded> decoded;
        std::erase_if(decoded, [](const auto& entry) {
            return entry.second.frames.expired();
        });
        AnimatedTexture animation;
        const auto stamp = file_stamp(path);
        auto& cached = decoded[path];
        if (stamp && *stamp == cached.stamp)
            animation.frames = cached.frames.lock();
        if (!animation.frames) {
            const auto file = path_to_utf8(path);
            animation.frames.reset(IMG_LoadAnimation(file.c_str()), IMG_FreeAnimation);
            cached = {stamp ? *stamp : FileStamp{}, animation.frames};
        }
        if (const auto& frames = animation.frames; frames && frames->count > 0) {
            animation.texture.reset(SDL_CreateTexture(renderer_, frames->frames[0]->format, SDL_TEXTUREACCESS_STREAMING,
                                                      frames->w, frames->h));
            SDL_SetTextureBlendMode(animation.texture.get(), SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(animation.texture.get(), SDL_SCALEMODE_LINEAR);
            animation.frame_ends_ms.reserve(static_cast<std::size_t>(frames->count));
            std::uint64_t duration = 0;
            for (int index = 0; index < frames->count; ++index) {
                duration += frames->delays[index] > 0 ? frames->delays[index] : 100;
                animation.frame_ends_ms.push_back(duration);
            }
        }
        found = animations_.emplace(path, std::move(animation)).first;
    }
    auto& animation = found->second;
    if (!animation.texture || animation.frame_ends_ms.empty())
        return nullptr;
    const auto elapsed = now_ms % animation.frame_ends_ms.back();
    const auto frame = static_cast<std::size_t>(
        std::upper_bound(animation.frame_ends_ms.begin(), animation.frame_ends_ms.end(), elapsed) -
        animation.frame_ends_ms.begin());
    next_frame_ms_ = std::min(next_frame_ms_, now_ms + animation.frame_ends_ms[frame] - elapsed);
    if (animation.current_frame != frame) {
        const auto* surface = animation.frames->frames[frame];
        if (!SDL_UpdateTexture(animation.texture.get(), nullptr, surface->pixels, surface->pitch)) {
            return nullptr;
        }
        animation.current_frame = frame;
    }
    return animation.texture.get();
}

void UiAssets::clear() {
    animations_.clear();
    textures_.clear();
    ui_textures_.clear();
    widget_textures_.clear();
}

SDL_Texture* UiAssets::named(NamedTextures& textures, const char* directory, const std::string_view name) {
    if (const auto found = textures.find(name); found != textures.end())
        return found->second;
    auto* const loaded = texture(asset_root_ / directory / name, SDL_SCALEMODE_NEAREST);
    textures.emplace(name, loaded);
    return loaded;
}

SDL_Texture* UiAssets::texture(const std::filesystem::path& path, const SDL_ScaleMode scale_mode) {
    const auto key = path.lexically_normal();
    auto [found, inserted] = textures_.try_emplace(key);
    if (inserted && key.is_absolute()) {
        const auto file = path_to_utf8(key);
        found->second.reset(IMG_LoadTexture(renderer_, file.c_str()));
        if (found->second)
            SDL_SetTextureScaleMode(found->second.get(), scale_mode);
    }
    return found->second.get();
}

SDL_Texture* UiAssets::cached(const std::filesystem::path& path) const {
    const auto found = textures_.find(path.lexically_normal());
    return found == textures_.end() ? nullptr : found->second.get();
}

void UiAssets::discard(const std::filesystem::path& path) {
    // Frames live beside the widgets, so a discarded texture may also be cached by name.
    if (const auto key = path.lexically_normal(); textures_.erase(key) != 0) {
        ui_textures_.clear();
        widget_textures_.clear();
    }
}

bool textured_button(SDL_Texture* normal, SDL_Texture* highlighted, const char* id, const char* label, ImVec2 size) {
    const bool clicked = ImGui::InvisibleButton(id, size);
    const auto min = ImGui::GetItemRectMin();
    const auto max = ImGui::GetItemRectMax();
    const auto text_size = ImGui::CalcTextSize(label);
    auto* draw = ImGui::GetWindowDrawList();
    // One button texel matches one font pixel, as in the game.
    draw_nine_slice(draw, ImGui::IsItemHovered() ? highlighted : normal, min, max, button_slice,
                    text_shadow_offset(ImGui::GetFont(), ImGui::GetFontSize()));
    draw_shadowed_text(
        draw, {min.x + (((max.x - min.x) - text_size.x) * 0.5F), min.y + (((max.y - min.y) - text_size.y) * 0.5F)},
        ImGui::GetColorU32(ImGuiCol_Text), label);
    return clicked;
}

bool textured_slider(SDL_Texture* normal, SDL_Texture* handle, SDL_Texture* handle_highlighted, const char* id,
                     float* value, float min_value, float max_value) {
    constexpr float handle_width = 8.0F;
    const float width = std::max(ImGui::GetContentRegionAvail().x, handle_width);
    const float track_width = std::max(width - handle_width, 0.0F);
    const float value_range = max_value - min_value;
    const bool pressed = ImGui::InvisibleButton(id, {width, 20.0F});
    const auto min = ImGui::GetItemRectMin();
    const auto max = ImGui::GetItemRectMax();
    const bool active = ImGui::IsItemActive();
    if ((active || pressed) && track_width > 0.0F && value_range > 0.0F) {
        *value = min_value + std::clamp((ImGui::GetIO().MousePos.x - min.x) / track_width, 0.0F, 1.0F) * value_range;
    }
    const float normalized = value_range > 0.0F ? std::clamp((*value - min_value) / value_range, 0.0F, 1.0F) : 0.0F;
    const float handle_x = min.x + (normalized * track_width);
    const bool handle_hovered = ImGui::IsMouseHoveringRect({handle_x, min.y}, {handle_x + handle_width, max.y});
    auto* draw = ImGui::GetWindowDrawList();
    draw_nine_slice(draw, normal, min, max, frame_slice, (max.y - min.y) / 20.0F);
    draw->AddImage(active || handle_hovered ? handle_highlighted : handle, {handle_x, min.y},
                   {handle_x + handle_width, max.y});
    const auto value_text = std::format("{:.2g}", *value);
    const auto text_size = ImGui::CalcTextSize(value_text.c_str());
    draw_shadowed_text(draw,
                       {min.x + ((max.x - min.x - text_size.x) * 0.5F), min.y + ((max.y - min.y - text_size.y) * 0.5F)},
                       IM_COL32(255, 255, 255, 255), value_text.c_str());
    return pressed || active;
}

bool textured_checkbox(SDL_Texture* normal, SDL_Texture* highlighted, SDL_Texture* selected,
                       SDL_Texture* selected_highlighted, const char* id, const char* label, bool* value,
                       const bool keep_selected, const ImVec4 text_color) {
    const bool clicked = ImGui::InvisibleButton(id, {20.0F, 20.0F});
    if (clicked) {
        *value = keep_selected ? true : !*value;
    }
    const bool hovered = ImGui::IsItemHovered();
    const auto min = ImGui::GetItemRectMin();
    const auto max = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddImage(
        *value ? (hovered ? selected_highlighted : selected) : (hovered ? highlighted : normal), min, max);
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, text_color);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    return clicked;
}

bool textured_input(SDL_Texture* normal, SDL_Texture* highlighted, const char* id, char* value, std::size_t size) {
    const auto min = ImGui::GetCursorScreenPos();
    const auto max = ImVec2{min.x + ImGui::GetContentRegionAvail().x, min.y + ImGui::GetFrameHeight()};
    auto* draw = ImGui::GetWindowDrawList();
    draw_nine_slice(draw, ImGui::IsMouseHoveringRect(min, max) ? highlighted : normal, min, max, frame_slice,
                    text_shadow_offset(ImGui::GetFont(), ImGui::GetFontSize()));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, {0.0F, 0.0F, 0.0F, 0.0F});
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, {0.0F, 0.0F, 0.0F, 0.0F});
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, {0.0F, 0.0F, 0.0F, 0.0F});
    ImGui::PushStyleColor(ImGuiCol_Text, {1.0F, 1.0F, 1.0F, 1.0F});
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0F);
    const bool changed = ImGui::InputText(id, value, size);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
    return changed;
}

bool minecraft_button(UiAssets& assets, const char* id, const char* label, const ImVec2 size) {
    return textured_button(assets.widget("button.png"), assets.widget("button_highlighted.png"), id, label, size);
}

bool minecraft_checkbox(UiAssets& assets, const char* id, const char* label, bool* value, const bool keep_selected,
                        const ImVec4 text_color) {
    return textured_checkbox(assets.widget("checkbox.png"), assets.widget("checkbox_highlighted.png"),
                             assets.widget("checkbox_selected.png"), assets.widget("checkbox_selected_highlighted.png"),
                             id, label, value, keep_selected, text_color);
}

}  // namespace beacon
