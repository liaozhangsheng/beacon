#pragma once

#include <beacon/app/persistence.hpp>

#include <SDL3/SDL.h>
#include <imgui.h>

#include <string>

namespace beacon {

int overlay_min_height(float extra_padding = 0.0F, float scale = 1.0F);

class WindowContext {
public:
    WindowContext(const std::string& title, const WindowSettings& settings, bool overlay, float scale = 1.0F,
                  bool overlay_transparent = true);
    ~WindowContext();
    WindowContext(const WindowContext&) = delete;
    WindowContext& operator=(const WindowContext&) = delete;

    [[nodiscard]] bool ready() const {
        return ready_;
    }
    [[nodiscard]] SDL_WindowID id() const;
    [[nodiscard]] SDL_Window* window() const {
        return window_;
    }
    [[nodiscard]] SDL_Renderer* renderer() const {
        return renderer_;
    }
    void process_event(const SDL_Event& event);
    void make_current() const;
    void set_scale(float scale);

private:
    bool overlay_ = false;
    bool dragging_ = false;
    SDL_MouseButtonFlags mouse_buttons_ = 0;
    bool ready_ = false;
    bool platform_ready_ = false;
    bool renderer_ready_ = false;
#if defined(_WIN32)
    float drag_start_mouse_x_ = 0.0F;
    float drag_start_mouse_y_ = 0.0F;
    int drag_start_window_x_ = 0;
    int drag_start_window_y_ = 0;
#endif
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    ImGuiContext* context_ = nullptr;
    float scale_ = 1.0F;
    ImGuiStyle base_style_{};
};

}  // namespace beacon
