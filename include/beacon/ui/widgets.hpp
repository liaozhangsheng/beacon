#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <SDL3/SDL.h>
#include <imgui.h>

struct IMG_Animation;

namespace beacon {

struct SdlTextureDeleter {
    void operator()(SDL_Texture* texture) const noexcept {
        if (texture != nullptr) {
            SDL_DestroyTexture(texture);
        }
    }
};

using SdlTexturePtr = std::unique_ptr<SDL_Texture, SdlTextureDeleter>;

class UiAssets {
private:
    struct AnimatedTexture {
        std::shared_ptr<IMG_Animation> frames;
        std::vector<std::uint64_t> frame_ends_ms;
        SdlTexturePtr texture;
        std::size_t current_frame = static_cast<std::size_t>(-1);
    };

public:
    explicit UiAssets(SDL_Renderer* renderer, const std::filesystem::path& asset_root)
        : renderer_(renderer), asset_root_(asset_root) {}
    ~UiAssets() = default;
    UiAssets(const UiAssets&) = delete;
    UiAssets& operator=(const UiAssets&) = delete;

    SDL_Texture* texture(const std::filesystem::path& path);
    SDL_Texture* ui(const char* name) {
        return texture(asset_root_ / "assets/ui" / name);
    }
    SDL_Texture* widget(const char* name) {
        return texture(asset_root_ / "assets/ui/widget" / name);
    }
    [[nodiscard]] SDL_Texture* cached(const std::filesystem::path& path) const;
    SDL_Texture* animated_frame(const std::filesystem::path& path, std::uint64_t now_ms);
    void begin_frame() {
        next_frame_ms_ = UINT64_MAX;
    }
    [[nodiscard]] std::uint64_t next_frame_ms() const {
        return next_frame_ms_;
    }
    void discard(const std::filesystem::path& path);
    void clear();

private:
    std::uint64_t next_frame_ms_ = UINT64_MAX;
    SDL_Renderer* renderer_ = nullptr;
    std::filesystem::path asset_root_;
    std::unordered_map<std::filesystem::path, SdlTexturePtr> textures_;
    std::unordered_map<std::filesystem::path, AnimatedTexture> animations_;
};

void draw_subpixel_text(ImDrawList* draw, ImVec2 position, ImU32 color, const char* begin, const char* end = nullptr);

// Draw each wrapped line centered and return its line count for positioning the next row.
std::size_t draw_progress_text(ImDrawList* draw, const std::string& text, float center_x, float y, ImU32 color,
                               float wrap_width = 0.0F);

bool textured_button(SDL_Texture* normal, SDL_Texture* highlighted, const char* id, const char* label, ImVec2 size);
bool textured_slider(SDL_Texture* normal, SDL_Texture* handle, SDL_Texture* handle_highlighted, const char* id,
                     float* value, float min_value, float max_value);
bool textured_checkbox(SDL_Texture* normal, SDL_Texture* highlighted, SDL_Texture* selected,
                       SDL_Texture* selected_highlighted, const char* id, const char* label, bool* value,
                       bool keep_selected = false, ImVec4 text_color = {1.0F, 1.0F, 1.0F, 1.0F});
bool textured_input(SDL_Texture* normal, SDL_Texture* highlighted, const char* id, char* value, std::size_t size);

}  // namespace beacon
