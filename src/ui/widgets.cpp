#include <beacon/ui/widgets.hpp>
#include <beacon/io/file.hpp>
#include <beacon/ui/sizing.hpp>

#include <SDL3_image/SDL_image.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>

namespace beacon {

void draw_subpixel_text(ImDrawList* draw, const ImVec2 position, const ImU32 color, const char* begin,
                        const char* end) {
    const int first_vertex = draw->VtxBuffer.Size;
    draw->AddText(position, color, begin, end);
    // ImFont truncates the text origin. Restore its fraction so labels move with images.
    const ImVec2 offset{position.x - std::trunc(position.x), position.y - std::trunc(position.y)};
    for (int index = first_vertex; index < draw->VtxBuffer.Size; ++index) {
        draw->VtxBuffer[index].pos.x += offset.x;
        draw->VtxBuffer[index].pos.y += offset.y;
    }
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
}

SDL_Texture* UiAssets::texture(const std::filesystem::path& path) {
    const auto key = path.lexically_normal();
    auto [found, inserted] = textures_.try_emplace(key);
    if (inserted && key.is_absolute()) {
        const auto file = path_to_utf8(key);
        found->second.reset(IMG_LoadTexture(renderer_, file.c_str()));
    }
    return found->second.get();
}

SDL_Texture* UiAssets::cached(const std::filesystem::path& path) const {
    const auto found = textures_.find(path.lexically_normal());
    return found == textures_.end() ? nullptr : found->second.get();
}

void UiAssets::discard(const std::filesystem::path& path) {
    textures_.erase(path.lexically_normal());
}

bool textured_button(SDL_Texture* normal, SDL_Texture* highlighted, const char* id, const char* label, ImVec2 size) {
    const bool clicked = ImGui::InvisibleButton(id, size);
    const auto min = ImGui::GetItemRectMin();
    const auto max = ImGui::GetItemRectMax();
    const auto text_size = ImGui::CalcTextSize(label);
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddImage(ImGui::IsItemHovered() ? highlighted : normal, min, max);
    draw->AddText({min.x + (((max.x - min.x) - text_size.x) * 0.5F), min.y + (((max.y - min.y) - text_size.y) * 0.5F)},
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
    draw->AddImage(normal, min, max);
    draw->AddImage(active || handle_hovered ? handle_highlighted : handle, {handle_x, min.y},
                   {handle_x + handle_width, max.y});
    const auto value_text = std::format("{:.2g}", *value);
    const auto text_size = ImGui::CalcTextSize(value_text.c_str());
    draw->AddText({min.x + ((max.x - min.x - text_size.x) * 0.5F), min.y + ((max.y - min.y - text_size.y) * 0.5F)},
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
    draw->AddImage(ImGui::IsMouseHoveringRect(min, max) ? highlighted : normal, min, max);
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

}  // namespace beacon
