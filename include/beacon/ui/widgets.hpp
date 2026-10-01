#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <functional>
#include <string>
#include <string_view>
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

    // The scale mode applies when the texture is first loaded.
    SDL_Texture* texture(const std::filesystem::path& path, SDL_ScaleMode scale_mode = SDL_SCALEMODE_LINEAR);
    // Pixel-art GUI chrome is sampled with nearest filtering, like Minecraft's own GUI.
    SDL_Texture* ui(const std::string_view name) {
        return named(ui_textures_, "assets/ui", name);
    }
    SDL_Texture* widget(const std::string_view name) {
        return named(widget_textures_, "assets/ui/widget", name);
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
    struct NameHash {
        using is_transparent = void;
        std::size_t operator()(const std::string_view name) const noexcept {
            return std::hash<std::string_view>{}(name);
        }
    };
    // GUI chrome is looked up by name every frame; skip building and normalizing its path each time.
    using NamedTextures = std::unordered_map<std::string, SDL_Texture*, NameHash, std::equal_to<>>;
    SDL_Texture* named(NamedTextures& textures, const char* directory, std::string_view name);

    std::uint64_t next_frame_ms_ = UINT64_MAX;
    SDL_Renderer* renderer_ = nullptr;
    std::filesystem::path asset_root_;
    std::unordered_map<std::filesystem::path, SdlTexturePtr> textures_;
    std::unordered_map<std::filesystem::path, AnimatedTexture> animations_;
    NamedTextures ui_textures_;
    NamedTextures widget_textures_;
};

// Texel insets of a nine-slice texture: corners keep their size, edges and center stretch.
struct NineSlice {
    float left = 0.0F;
    float top = 0.0F;
    float right = 0.0F;
    float bottom = 0.0F;
};
inline constexpr NineSlice button_slice{2.0F, 2.0F, 2.0F, 3.0F};
inline constexpr NineSlice frame_slice{1.0F, 1.0F, 1.0F, 1.0F};

// Rounds a logical coordinate to the nearest framebuffer pixel.
float snap_to_pixel(float value);
// Draws texture into [min, max] with borders scaled by unit (logical size of one texel).
void draw_nine_slice(ImDrawList* draw, SDL_Texture* texture, ImVec2 min, ImVec2 max, NineSlice slice, float unit,
                     ImU32 tint = IM_COL32_WHITE);
// Recessed panel in the style of an inventory slot: dark top-left edge, light bottom-right edge.
void draw_inset_panel(ImDrawList* draw, ImVec2 min, ImVec2 max, float unit);

// Minecraft-style text: a one-font-pixel drop shadow at a quarter of the text brightness.
float text_shadow_offset(ImFont* font, float font_size);
ImU32 text_shadow_color(ImU32 color);
void draw_shadowed_text(ImDrawList* draw, ImFont* font, float font_size, ImVec2 position, ImU32 color,
                        const char* begin, const char* end = nullptr, float wrap_width = 0.0F);
void draw_shadowed_text(ImDrawList* draw, ImVec2 position, ImU32 color, const char* begin, const char* end = nullptr);
// Shadowed replacement for ImGui::TextUnformatted using ImGuiCol_Text.
void shadowed_text(const char* begin, const char* end = nullptr);
// Trims text to max_width at a UTF-8 boundary and appends "...".
std::string ellipsize_text(const std::string& text, float max_width);
// Vanilla-style hover tooltip; multi-line text is supported.
void minecraft_tooltip(const char* text);
// Keeps the fractional origin so labels move smoothly with scrolling images.
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

// The vanilla widgets, drawn with the bundled widget textures.
bool minecraft_button(UiAssets& assets, const char* id, const char* label, ImVec2 size);
bool minecraft_checkbox(UiAssets& assets, const char* id, const char* label, bool* value, bool keep_selected = false,
                        ImVec4 text_color = {1.0F, 1.0F, 1.0F, 1.0F});

}  // namespace beacon
