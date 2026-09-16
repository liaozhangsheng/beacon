#include "desktop_renderer_internal.hpp"

#include <algorithm>
#include <cmath>

#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

namespace beacon {
namespace {

constexpr float completion_view_delay = 0.5F;

}  // namespace

[[nodiscard]] bool WindowRenderer::Impl::ready() const {
    return ready_;
}

[[nodiscard]] SDL_WindowID WindowRenderer::Impl::id() const {
    return context_.id();
}

[[nodiscard]] SDL_Renderer* WindowRenderer::Impl::renderer() const {
    return context_.renderer();
}

std::unique_ptr<WindowRenderer::Impl> WindowRenderer::Impl::recreate(const bool overlay_transparent) const {
    WindowSettings settings;
    SDL_GetWindowPosition(context_.window(), &settings.x, &settings.y);
    SDL_GetWindowSize(context_.window(), &settings.width, &settings.height);
    return std::make_unique<Impl>(title_, settings, overlay_, asset_root_, view_, scale_, overlay_transparent);
}

void WindowRenderer::Impl::set_scale(const float scale) {
    if (scale_ == scale) {
        return;
    }
    scale_ = scale;
    context_.set_scale(scale_);
    if (overlay_ && !completion_view_) {
        SDL_SetWindowMinimumSize(context_.window(), 0, overlay_min_height(0.0F, scale_));
    }
}

void WindowRenderer::Impl::process_event(const SDL_Event& event) {
    context_.process_event(event);
    if (SDL_GetWindowFromEvent(&event) == context_.window()) {
        // ImGui needs follow-up frames for queued input, hover delays and widget transitions.
        interactive_until_ = SDL_GetTicks() + 750;
    }
}

void WindowRenderer::Impl::prepare(const std::shared_ptr<const PublishedState>& state,
                                   const std::shared_ptr<const PlayerCard>& player_card) {
    prepare_profile(player_card);
    prepare_template(state);
    prepare_layout(state);
}

void WindowRenderer::Impl::render(const std::shared_ptr<const PublishedState>& state,
                                  const std::optional<Error>& runtime_error, Settings* settings,
                                  std::optional<Error>* settings_error,
                                  const std::vector<std::filesystem::path>* template_options, bool* apply_settings,
                                  Runtime* runtime) {
    context_.make_current();
    if (settings != nullptr) {
        const auto requested_scale = overlay_ ? settings->overlay_window_scale : settings->main_window_scale;
        set_scale(std::clamp(requested_scale, min_window_scale, max_window_scale));
    }
    manual_runtime_ = runtime;
    continuous_animation_ = false;
    assets_->begin_frame();
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    if (overlay_ && state) {
        completion_.update(*state);
        const bool complete = is_complete(*state);
        if (!complete || completion_.any_active()) {
            completion_started_.reset();
        } else if (!completion_started_) {
            completion_started_ = static_cast<float>(ImGui::GetTime());
        }
        set_completion_view(complete && !completion_.any_active() &&
                            static_cast<float>(ImGui::GetTime()) - *completion_started_ >= completion_view_delay);
    }

    auto* const viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    const auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                       ImGuiWindowFlags_NoBringToFrontOnFocus | (overlay_ ? ImGuiWindowFlags_NoBackground : 0);
    ImGui::PushFont(nullptr, ui::font_size(scale_));
    const bool transparent_background = overlay_ && (settings ? settings->overlay_transparent : overlay_transparent_);
    const auto background = [&] {
        if (transparent_background) {
            return ImVec4{0.0F, 0.0F, 0.0F, 0.0F};
        }
        if (!settings) {
            return overlay_
                       ? ImVec4{default_overlay_window_background_color[0], default_overlay_window_background_color[1],
                                default_overlay_window_background_color[2], 1.0F}
                       : ImVec4{0.2F, 0.2F, 0.2F, 1.0F};
        }
        const auto& color =
            overlay_ ? settings->overlay_window_background_color : settings->main_window_background_color;
        return ImVec4{color[0], color[1], color[2], 1.0F};
    }();
    ImGui::PushStyleColor(ImGuiCol_WindowBg, background);
    const bool window_visible = ImGui::Begin(overlay_ ? "Overlay##beacon" : "Tracker##beacon", nullptr, flags);
    if (window_visible) {
        render_progress_view(state, runtime_error, settings ? settings->overlay_scroll_speed : 60.0F,
                             settings != nullptr && settings->overlay_scroll_right);
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopFont();

    if (!overlay_ && settings != nullptr) {
        settings_panel_.render(viewport, *settings, settings_error, template_options, apply_settings, *assets_);
    }

    continuous_animation_ |= (state && (completion_.any_active() || completion_view_ ||
                                        (completion_started_.has_value() && !completion_view_))) ||
                             ImGui::IsAnyItemActive() || ImGui::GetIO().WantTextInput;
    ImGui::Render();
    const auto& io = ImGui::GetIO();
    SDL_SetRenderScale(context_.renderer(), io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
    const auto to_byte = [](const float value) {
        return static_cast<Uint8>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
    };
    SDL_SetRenderDrawColor(context_.renderer(), to_byte(background.x), to_byte(background.y), to_byte(background.z),
                           to_byte(background.w));
    SDL_RenderClear(context_.renderer());
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), context_.renderer());
    SDL_RenderPresent(context_.renderer());
}

std::uint64_t WindowRenderer::Impl::refresh_delay_ms() const {
    const auto now = SDL_GetTicks();
    if (continuous_animation_ || now < interactive_until_) {
        // VSync may queue a present without blocking; always keep a CPU frame budget.
        return overlay_ ? 16 : 33;
    }
    const auto next = assets_->next_frame_ms();
    return next <= now ? 1 : std::min<std::uint64_t>(1000, next - now);
}

std::uint64_t WindowRenderer::refresh_delay_ms() const {
    return impl_->refresh_delay_ms();
}

WindowRenderer::WindowRenderer(const std::string& title, const WindowSettings& settings, const bool overlay,
                               const std::filesystem::path& asset_root, const ProgressViewModel& view,
                               const float scale, const bool overlay_transparent)
    : impl_(std::make_unique<Impl>(title, settings, overlay, asset_root, view, scale, overlay_transparent)) {}

bool WindowRenderer::overlay_transparent() const {
    return impl_->overlay_transparent();
}

bool WindowRenderer::set_overlay_transparent(const bool enabled) {
    if (!impl_->overlay() || impl_->overlay_transparent() == enabled) {
        return true;
    }

    auto* const old_window = SDL_GetWindowFromID(impl_->id());
    const auto old_flags = SDL_GetWindowFlags(old_window);
    auto replacement = impl_->recreate(enabled);
    if (!replacement->ready()) {
        return false;
    }

    impl_ = std::move(replacement);
    auto* const new_window = SDL_GetWindowFromID(impl_->id());
    if ((old_flags & SDL_WINDOW_HIDDEN) != 0) {
        SDL_HideWindow(new_window);
    } else if ((old_flags & SDL_WINDOW_MINIMIZED) != 0) {
        SDL_MinimizeWindow(new_window);
    }
    return true;
}

WindowRenderer::~WindowRenderer() = default;

bool WindowRenderer::ready() const {
    return impl_->ready();
}

SDL_WindowID WindowRenderer::id() const {
    return impl_->id();
}

SDL_Renderer* WindowRenderer::renderer() const {
    return impl_->renderer();
}

void WindowRenderer::process_event(const SDL_Event& event) {
    impl_->process_event(event);
}

void WindowRenderer::prepare(const std::shared_ptr<const PublishedState>& state,
                             const std::shared_ptr<const PlayerCard>& player_card) {
    impl_->prepare(state, player_card);
}

void WindowRenderer::render(const std::shared_ptr<const PublishedState>& state,
                            const std::optional<Error>& runtime_error, Settings* settings,
                            std::optional<Error>* settings_error,
                            const std::vector<std::filesystem::path>* template_options, bool* apply_settings,
                            Runtime* runtime) {
    impl_->render(state, runtime_error, settings, settings_error, template_options, apply_settings, runtime);
}

}  // namespace beacon
