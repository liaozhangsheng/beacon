#pragma once

#include <beacon/app/persistence.hpp>

#include <SDL3/SDL.h>
#include <imgui.h>

#include <string>

namespace beacon {

class WindowContext {
public:
    WindowContext(const std::string& title, const WindowSettings& settings, bool overlay, float scale = 1.0F,
                  bool overlay_transparent = true, bool initially_visible = true);
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
    // Makes this window's GL context current, if its renderer uses one. Call before building a frame.
    void activate_renderer() const;
    void set_scale(float scale);
    // Enforces a height floor without shrinking a taller window or limiting its width.
    void ensure_minimum_height(int height);

private:
    bool overlay_ = false;
    float scale_ = 1.0F;
    ImGuiStyle base_style_{};

    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_GLContext gl_context_ = nullptr;
    ImGuiContext* context_ = nullptr;
    bool ready_ = false;
    bool platform_ready_ = false;
    bool renderer_ready_ = false;

    bool dragging_ = false;
    SDL_MouseButtonFlags mouse_buttons_ = 0;
#if defined(_WIN32)
    float drag_start_mouse_x_ = 0.0F;
    float drag_start_mouse_y_ = 0.0F;
    int drag_start_window_x_ = 0;
    int drag_start_window_y_ = 0;
#endif
};

}  // namespace beacon
