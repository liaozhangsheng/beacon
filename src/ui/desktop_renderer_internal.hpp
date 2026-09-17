#pragma once

#include "desktop_renderer.hpp"
#include "completion_animation.hpp"
#include "settings_panel.hpp"
#include "window_context.hpp"

#include <beacon/ui/carousel.hpp>
#include <beacon/ui/layout_metrics.hpp>
#include <beacon/ui/profile.hpp>
#include <beacon/ui/progress.hpp>
#include <beacon/ui/sizing.hpp>
#include <beacon/ui/widgets.hpp>

#include <SDL3/SDL.h>
#include <imgui.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace beacon {

class WindowRenderer::Impl {
public:
    Impl(const std::string& title, const WindowSettings& settings, bool overlay,
         const std::filesystem::path& asset_root, const ProgressViewModel& view, float scale = 1.0F,
         bool overlay_transparent = true, bool initially_visible = true);

    [[nodiscard]] bool ready() const;
    [[nodiscard]] SDL_WindowID id() const;
    [[nodiscard]] SDL_Renderer* renderer() const;
    [[nodiscard]] bool overlay() const {
        return overlay_;
    }
    [[nodiscard]] bool overlay_transparent() const {
        return overlay_transparent_;
    }
    [[nodiscard]] std::unique_ptr<Impl> recreate(bool overlay_transparent) const;

    void set_scale(float scale);

    [[nodiscard]] std::uint64_t refresh_delay_ms() const;

    void process_event(const SDL_Event& event);
    void prepare(const std::shared_ptr<const PublishedState>& state,
                 const std::shared_ptr<const PlayerCard>& player_card);
    void render(const std::shared_ptr<const PublishedState>& state, const std::optional<Error>& runtime_error,
                Settings* settings, std::optional<Error>* settings_error,
                const std::vector<std::filesystem::path>* template_options, bool* apply_settings, Runtime* runtime);

private:
    void prepare_profile(const std::shared_ptr<const PlayerCard>& player_card);
    void prepare_template(const std::shared_ptr<const PublishedState>& state);
    void build_atlas(const std::vector<std::filesystem::path>& paths);
    void prepare_layout(const std::shared_ptr<const PublishedState>& state);
    void update_progress_glows(const PublishedState& state);
    void draw_glow(ImDrawList* draw, ImVec2 frame_min, float frame_size, float brightness, float phase,
                   float scale = 1.5F) const;
    void draw_progress_glow(ImDrawList* draw, std::uint32_t node, ImVec2 frame_min, float icon_size) const;

    void render_progress_view(const std::shared_ptr<const PublishedState>& state,
                              const std::optional<Error>& runtime_error, float scroll_speed, bool scroll_right);
    void set_completion_view(bool complete);
    void draw_completion_view(const PublishedState& state);
    void draw_player_card(const PublishedState& state, float center_y) const;
    void draw_completion_progress(const ProgressStats& stats, const std::string& progress_text,
                                  const std::string& percentage_text, float center_y) const;
    void draw_header(const PublishedState& state);
    float draw_main_controls(ImVec2 origin, float footer_top, float status_height);
    [[nodiscard]] float status_footer_height() const;
    void draw_status_footer(ImVec2 origin, ImVec2 available, float footer_top) const;

    struct AtlasRegion {
        ImVec2 uv_min;
        ImVec2 uv_max;
    };

    struct ItemLayout {
        float cell_x = 0.0F;
        float cell_y = 0.0F;
        float cell_width = 0.0F;
        float cell_height = 0.0F;
        float frame_x = 0.0F;
        float frame_y = 0.0F;
        float frame_size = 0.0F;
        float icon_x = 0.0F;
        float icon_y = 0.0F;
        float icon_size = 0.0F;
        bool show_frame = true;
        bool draw_glow = false;
        bool stats = false;
        bool scrollable = false;
        bool collection_item = false;
    };

    static bool is_stats_group(const LayoutGroup& group, const PublishedState& state,
                               std::span<const std::uint32_t> nodes);
    void draw_node_icon(const PublishedState& state, ImDrawList* draw, std::uint32_t node, ImVec2 frame_position,
                        float frame_size, ImVec2 icon_position, float icon_size, bool show_frame, bool draw_glow,
                        ImU32 icon_tint = IM_COL32(255, 255, 255, 255), ImDrawListSplitter* splitter = nullptr) const;
    void draw_item(const PublishedState& state, const LayoutGroup& group, ImDrawList* draw, std::uint32_t index,
                   const ItemLayout& item, ImDrawListSplitter& splitter);

    void draw_overlay_layout(const PublishedState& state, ImVec2 origin, ImVec2 available, float body_height,
                             float overlay_scroll_speed, bool scroll_right);
    void draw_main_layout(const PublishedState& state, ImVec2 origin, ImVec2 available, float body_height);
    void draw_responsive_layout(const PublishedState& state, float overlay_scroll_speed, bool scroll_right);
    void load_cjk_font();

    [[nodiscard]] float from_icon(float original_size) const {
        return ui::from_icon(original_size, scale_);
    }
    [[nodiscard]] float icon_size() const {
        return ui::icon_size(scale_);
    }

    IconFiles icon_files_;
    std::string title_;
    std::filesystem::path asset_root_;
    bool overlay_ = false;
    bool overlay_transparent_ = true;
    bool ready_ = false;
    float scale_ = 1.0F;
    mutable bool continuous_animation_ = false;
    std::uint64_t interactive_until_ = 0;
    WindowContext context_;
    mutable std::optional<UiAssets> assets_;
    SDL_Texture* glow_texture_ = nullptr;
    std::vector<float> glow_brightness_;
    float glow_animation_time_ = 0.0F;
    float glow_update_elapsed_ = 0.0F;
    CompletionAnimator completion_;
    CompletionFireworks fireworks_;
    bool completion_view_ = false;
    std::optional<float> completion_started_;
    std::vector<CarouselState> overlay_carousels_;
    const ProgressViewModel& view_;
    std::shared_ptr<const CompiledTemplate> compiled_;
    const Layout* layout_ = nullptr;
    SettingsPanel settings_panel_;
    std::shared_ptr<const PlayerCard> player_card_;
    SdlTexturePtr remote_avatar_texture_;
    SDL_Texture* avatar_texture_ = nullptr;
    SdlTexturePtr atlas_texture_;
    std::unordered_map<std::filesystem::path, AtlasRegion> atlas_regions_;
    Runtime* manual_runtime_ = nullptr;
    std::string control_message_;
    std::string footer_message_;
    ImVec4 footer_color_{1.0F, 0.65F, 0.25F, 1.0F};
};

}  // namespace beacon
