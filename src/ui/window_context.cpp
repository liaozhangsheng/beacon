#include "window_context.hpp"

#include <beacon/ui/sizing.hpp>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>
#include <SDL3/SDL_mouse.h>

#include <algorithm>
#include <cmath>

namespace beacon {
namespace {

SDL_HitTestResult overlay_hit_test(SDL_Window* window, const SDL_Point* point, void* user_data) {
    int width = 0;
    int height = 0;
    SDL_GetWindowSize(window, &width, &height);
    const auto scale = user_data == nullptr ? 1.0F : *static_cast<const float*>(user_data);
    const int edge = static_cast<int>(std::ceil(ui::from_icon(8.0F, scale)));
    const bool left = point->x < edge;
    const bool right = point->x >= width - edge;
    const bool top = point->y < edge;
    const bool bottom = point->y >= height - edge;
    if (top && left)
        return SDL_HITTEST_RESIZE_TOPLEFT;
    if (top && right)
        return SDL_HITTEST_RESIZE_TOPRIGHT;
    if (bottom && left)
        return SDL_HITTEST_RESIZE_BOTTOMLEFT;
    if (bottom && right)
        return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
    if (top)
        return SDL_HITTEST_RESIZE_TOP;
    if (bottom)
        return SDL_HITTEST_RESIZE_BOTTOM;
    if (left)
        return SDL_HITTEST_RESIZE_LEFT;
    if (right)
        return SDL_HITTEST_RESIZE_RIGHT;
#if defined(_WIN32)
    // A draggable hit-test enters Windows' modal move loop and pauses the render loop.
    return SDL_HITTEST_NORMAL;
#else
    return SDL_HITTEST_DRAGGABLE;
#endif
}

}  // namespace

int overlay_min_height(const float extra_padding, const float scale) {
    const auto frame_size = ui::progress_frame_size(scale);
    const auto font_size = ui::font_size(scale);
    const float row_content = frame_size + (frame_size * 2.0F);
    constexpr float title_and_progress_lines = 5.0F;
    const float row_gaps_and_padding = frame_size;
    return static_cast<int>(std::ceil(row_content + (font_size * title_and_progress_lines) + row_gaps_and_padding +
                                      (ui::from_icon(extra_padding, scale) * 2.0F)));
}

WindowContext::WindowContext(const std::string& title, const WindowSettings& settings, const bool overlay,
                             const float scale, const bool overlay_transparent, const bool initially_visible)
    : overlay_(overlay), scale_(scale) {
    const auto overlay_focus_flags =
#if defined(__linux__)
        0u;
#else
        SDL_WINDOW_NOT_FOCUSABLE;
#endif
    const auto overlay_flags =
        SDL_WINDOW_BORDERLESS | overlay_focus_flags | (overlay_transparent ? SDL_WINDOW_TRANSPARENT : 0);
    const auto visibility_flags = initially_visible ? 0u : SDL_WINDOW_HIDDEN;
    const auto flags =
        SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE | visibility_flags | (overlay ? overlay_flags : 0);
    const int height = overlay ? std::max(settings.height, overlay_min_height(0.0F, scale)) : settings.height;
    window_ = SDL_CreateWindow(title.c_str(), settings.width, height, flags);
    if (window_ == nullptr)
        return;
    if (overlay && !SDL_SetWindowMinimumSize(window_, 0, overlay_min_height(0.0F, scale))) {
        SDL_Log("Overlay minimum size unavailable: %s", SDL_GetError());
    }
    if (overlay && !SDL_SetWindowHitTest(window_, overlay_hit_test, &scale_)) {
        SDL_Log("Overlay hit testing unavailable: %s", SDL_GetError());
    }
    SDL_SetWindowPosition(window_, settings.x, settings.y);
    renderer_ = SDL_CreateRenderer(window_, nullptr);
    if (renderer_ == nullptr)
        return;
    // Only the overlay paces the shared UI thread; the tracker has a timer.
    SDL_SetRenderVSync(renderer_, overlay ? 1 : 0);
    context_ = ImGui::CreateContext();
    ImGui::SetCurrentContext(context_);
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::StyleColorsDark();
    base_style_ = ImGui::GetStyle();
    if (overlay_)
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    set_scale(scale);
    platform_ready_ = ImGui_ImplSDL3_InitForSDLRenderer(window_, renderer_);
    if (!platform_ready_)
        return;
    // Capture is global to SDL. Manage it from window events so an idle context's
    // NewFrame cannot release the other window's active drag.
    ImGui_ImplSDL3_SetMouseCaptureMode(ImGui_ImplSDL3_MouseCaptureMode_Disabled);
    renderer_ready_ = ImGui_ImplSDLRenderer3_Init(renderer_);
    if (!renderer_ready_)
        return;
    ready_ = true;
}

WindowContext::~WindowContext() {
    if (mouse_buttons_ != 0) {
        SDL_CaptureMouse(false);
        dragging_ = false;
    }
    make_current();
    if (renderer_ready_)
        ImGui_ImplSDLRenderer3_Shutdown();
    if (platform_ready_)
        ImGui_ImplSDL3_Shutdown();
    if (context_ != nullptr)
        ImGui::DestroyContext(context_);
    SDL_DestroyRenderer(renderer_);
    SDL_DestroyWindow(window_);
}

SDL_WindowID WindowContext::id() const {
    return SDL_GetWindowID(window_);
}

void WindowContext::make_current() const {
    if (context_ != nullptr)
        ImGui::SetCurrentContext(context_);
}

void WindowContext::set_scale(const float scale) {
    scale_ = scale;
    make_current();
    auto& style = ImGui::GetStyle();
    style = base_style_;
    style.ScaleAllSizes(scale_);
    style.FontSizeBase = ui::font_size(scale_);
}

void WindowContext::process_event(const SDL_Event& event) {
    make_current();
    if ((event.type == SDL_EVENT_WINDOW_FOCUS_LOST || event.type == SDL_EVENT_WINDOW_HIDDEN) &&
        event.window.windowID == id()) {
        dragging_ = false;
        if (mouse_buttons_ != 0) {
            mouse_buttons_ = 0;
            SDL_CaptureMouse(false);
        }
    }
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.windowID == id()) {
        mouse_buttons_ |= SDL_BUTTON_MASK(event.button.button);
        SDL_CaptureMouse(true);
    }
    if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && mouse_buttons_ != 0) {
        mouse_buttons_ &= ~SDL_BUTTON_MASK(event.button.button);
        if (mouse_buttons_ == 0)
            SDL_CaptureMouse(false);
    }
#if defined(_WIN32)
    const bool mouse_event = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP ||
                             event.type == SDL_EVENT_MOUSE_MOTION;
    const bool belongs =
        !mouse_event || (event.type == SDL_EVENT_MOUSE_MOTION ? event.motion.windowID : event.button.windowID) == id();
    if (overlay_ && (belongs || dragging_)) {
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT) {
            dragging_ = true;
            SDL_GetGlobalMouseState(&drag_start_mouse_x_, &drag_start_mouse_y_);
            SDL_GetWindowPosition(window_, &drag_start_window_x_, &drag_start_window_y_);
        }
        if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT) {
            dragging_ = false;
        }
        if (event.type == SDL_EVENT_MOUSE_MOTION && dragging_) {
            float mouse_x = 0.0F;
            float mouse_y = 0.0F;
            SDL_GetGlobalMouseState(&mouse_x, &mouse_y);
            SDL_SetWindowPosition(window_,
                                  drag_start_window_x_ + static_cast<int>(std::lround(mouse_x - drag_start_mouse_x_)),
                                  drag_start_window_y_ + static_cast<int>(std::lround(mouse_y - drag_start_mouse_y_)));
        }
    }
#endif
    ImGui_ImplSDL3_ProcessEvent(&event);
}

}  // namespace beacon
