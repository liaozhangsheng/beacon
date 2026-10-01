#include <beacon/ui/desktop.hpp>
#include <beacon/minecraft/process.hpp>
#include <beacon/ui/display.hpp>
#include <beacon/ui/profile.hpp>
#include <beacon/ui/progress.hpp>
#include <beacon/io/file.hpp>
#include <beacon/update/layout.hpp>
#include <beacon/update/version.hpp>
#include "desktop_renderer.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
    #define NOMINMAX
    #include <windows.h>
#else
    #include <unistd.h>
#endif

namespace beacon {
namespace {

class SdlSession {
public:
    SdlSession() {
        const bool already_initialized = SDL_WasInit(SDL_INIT_VIDEO) != 0;
        ready_ = already_initialized || SDL_Init(SDL_INIT_VIDEO);
        owns_sdl_ = ready_ && !already_initialized;
    }
    ~SdlSession() {
        if (owns_sdl_)
            SDL_Quit();
    }
    [[nodiscard]] bool ready() const {
        return ready_;
    }

private:
    bool ready_ = false;
    bool owns_sdl_ = false;
};

void raise_overlay_window(SDL_Window* window) {
    if (!SDL_RaiseWindow(window))
        SDL_Log("Could not raise overlay window: %s", SDL_GetError());
}

SDL_Process* start_process(const std::vector<std::string>& arguments, const bool capture_stdout) {
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (const auto& argument : arguments)
        argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);

    const auto properties = SDL_CreateProperties();
    if (properties == 0)
        return nullptr;
    bool configured = SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv.data()) &&
                      SDL_SetBooleanProperty(properties, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, !capture_stdout);
    if (capture_stdout) {
        configured = configured &&
                     SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP) &&
                     SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_NULL);
    }
    auto* process = configured ? SDL_CreateProcessWithProperties(properties) : nullptr;
    SDL_DestroyProperties(properties);
    return process;
}

std::uint64_t current_process_id() {
#if defined(_WIN32)
    return GetCurrentProcessId();
#else
    return static_cast<std::uint64_t>(getpid());
#endif
}

}  // namespace

struct DesktopLoop::Impl {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr auto poll_interval = std::chrono::milliseconds(250);

    Impl(Runtime* runtime, Persistence* persistence, std::shared_ptr<const PublishedState> fixed_state,
         const Settings& settings, const std::filesystem::path& asset_root, const std::filesystem::path& install_root,
         const std::filesystem::path& data_root, const std::vector<std::filesystem::path>& template_options,
         const int frame_limit, std::optional<Error> initial_error)
        : runtime_(runtime), persistence_(persistence), fixed_state_(std::move(fixed_state)), asset_root_(asset_root),
          install_root_(install_root), data_root_(data_root), template_options_(template_options),
          current_settings_(settings), applied_settings_(settings), frame_limit_(frame_limit),
          runtime_error_(std::move(initial_error)),
          tracker_("Beacon Tracker", WindowSettings{}, false, asset_root_, view_, settings.main_window_scale),
          overlay_("Beacon Overlay", WindowSettings{.x = 120, .y = 120, .width = 1280, .height = 180}, true,
                   asset_root_, view_, settings.overlay_window_scale, settings.overlay_transparent,
                   settings.overlay_visible),
          profile_loader_(data_root_) {
        const auto initial_state = runtime_ ? runtime_->state() : fixed_state_;
        resource_loader_ =
            TemplateResourceLoader(settings.template_path, asset_root_,
                                   initial_state ? TemplateResources{initial_state->compiled,
                                                                     initial_state->localization, initial_state->layout}
                                                 : TemplateResources{});
        if (!tracker_.ready() || !overlay_.ready() || tracker_.renderer() == overlay_.renderer()) {
            running_ = false;
            return;
        }
        overlay_visible_ = current_settings_.overlay_visible;
        if (!overlay_visible_)
            SDL_HideWindow(SDL_GetWindowFromID(overlay_.id()));
        else
            raise_overlay_window(SDL_GetWindowFromID(overlay_.id()));
        next_poll_ = next_tracker_frame_ = next_overlay_frame_ = Clock::now();
        if (frame_limit_ == 0 && !fixed_state_)
            start_update_check();
        ready_ = true;
    }

    ~Impl() {
        if (update_check_ != nullptr) {
            if (!SDL_WaitProcess(update_check_, false, nullptr))
                SDL_KillProcess(update_check_, true);
            SDL_DestroyProcess(update_check_);
        }
    }

    [[nodiscard]] bool ready() const {
        return ready_;
    }

    [[nodiscard]] bool running() const {
        return running_;
    }

    void process_event(const SDL_Event& event) {
        if (!ready_ || !running_)
            return;
        const auto frame_started = Clock::now();
        if (event.type == SDL_EVENT_QUIT ||
            (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == tracker_.id())) {
            running_ = false;
        }
        if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == overlay_.id())
            current_settings_.overlay_visible = false;
        tracker_.process_event(event);
        overlay_.process_event(event);
        if (auto* window = SDL_GetWindowFromEvent(&event)) {
            const auto deadline = frame_started + std::chrono::milliseconds(33);
            if (SDL_GetWindowID(window) == tracker_.id())
                next_tracker_frame_ = std::min(next_tracker_frame_, deadline);
            if (SDL_GetWindowID(window) == overlay_.id())
                next_overlay_frame_ = std::min(next_overlay_frame_, deadline);
        }
    }

    bool iterate() {
        if (!ready_ || !running_)
            return false;
        const auto frame_started = Clock::now();
        poll_update_check();
        if (runtime_ != nullptr && frame_started >= next_poll_) {
            next_poll_ = frame_started + poll_interval;
            if (applied_settings_.auto_detect && current_settings_.auto_detect) {
                if (auto directory = foreground_minecraft_directory();
                    directory && *directory != applied_settings_.game_root) {
                    current_settings_.game_root = std::move(*directory);
                    apply_settings_ = true;
                }
            }
            (void)runtime_->poll_files();
        }
        if (runtime_ != nullptr)
            (void)runtime_->process_next(std::chrono::milliseconds(0));
        const auto state = runtime_ != nullptr ? runtime_->state() : fixed_state_;
        if (state)
            view_.update(*state);
        const auto player_card = profile_loader_.update(state ? state->run.player.uuid : std::string_view{});
        if (state != previous_state_ || player_card != previous_player_) {
            next_tracker_frame_ = next_overlay_frame_ = frame_started;
            previous_state_ = state;
            previous_player_ = player_card;
        }

        const auto tracker_flags = SDL_GetWindowFlags(SDL_GetWindowFromID(tracker_.id()));
        const bool tracker_visible = (tracker_flags & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED)) == 0;
        if (tracker_visible && frame_started >= next_tracker_frame_) {
            const auto render_started = Clock::now();
            tracker_.prepare(state, player_card);
            tracker_.render(state, runtime_error_, &current_settings_, &settings_error_, &template_options_,
                            &apply_settings_, runtime_, &update_notice_);
            if (update_notice_.apply_requested) {
                update_notice_.apply_requested = false;
                start_update_apply();
            }
            // A held pointer (dragging the settings book, a slider) runs at the display rate; otherwise cap the
            // tracker at 30 fps, or 5 fps in the background.
            const auto minimum_delay = tracker_.pointer_held()                         ? 0ULL
                                       : (tracker_flags & SDL_WINDOW_INPUT_FOCUS) != 0 ? 33ULL
                                                                                       : 200ULL;
            // Pace from the frame start so render time does not stretch the interval.
            next_tracker_frame_ =
                render_started +
                std::chrono::milliseconds(std::max<std::uint64_t>(minimum_delay, tracker_.refresh_delay_ms()));
        }
        if (overlay_.overlay_transparent() != current_settings_.overlay_transparent) {
            if (!overlay_.set_overlay_transparent(current_settings_.overlay_transparent)) {
                current_settings_.overlay_transparent = overlay_.overlay_transparent();
                apply_settings_ = false;
                settings_error_ = Error{.code = ErrorCode::Internal,
                                        .message = "cannot recreate overlay for transparency setting",
                                        .context = "overlay"};
            } else {
                next_overlay_frame_ = frame_started;
            }
        }
        if (apply_settings_) {
            apply_settings_ = false;
            auto candidate_loader = resource_loader_;
            ylt::expected<void, Error> applied;
            std::optional<RuntimeConfiguration> prepared;
            if (runtime_ == nullptr || persistence_ == nullptr) {
                applied = ylt::unexpected<Error>{
                    {.code = ErrorCode::Internal, .message = "runtime is unavailable", .context = "settings"}};
            } else {
                const bool language_only = current_settings_.template_path == applied_settings_.template_path &&
                                           current_settings_.language != applied_settings_.language;
                auto resources = candidate_loader.load(current_settings_.template_path, asset_root_,
                                                       current_settings_.language, language_only);
                if (!resources) {
                    applied = ylt::unexpected<Error>{std::move(resources.error())};
                } else {
                    auto candidate = Runtime::prepare_configuration(current_settings_.game_root, resources->compiled,
                                                                    resources->localization, resources->layout);
                    if (!candidate)
                        applied = ylt::unexpected<Error>{std::move(candidate.error())};
                    else
                        prepared = std::move(*candidate);
                }
            }
            if (applied)
                applied = persistence_->save_settings(current_settings_);
            if (!applied) {
                settings_error_ = std::move(applied.error());
            } else {
                if (prepared)
                    runtime_->commit_configuration(std::move(*prepared));
                resource_loader_ = std::move(candidate_loader);
                applied_settings_ = current_settings_;
                settings_error_.reset();
                runtime_error_.reset();
            }
        }
        if (overlay_visible_ != current_settings_.overlay_visible) {
            overlay_visible_ = current_settings_.overlay_visible;
            if (overlay_visible_) {
                auto* window = SDL_GetWindowFromID(overlay_.id());
                SDL_ShowWindow(window);
                raise_overlay_window(window);
            } else
                SDL_HideWindow(SDL_GetWindowFromID(overlay_.id()));
        }
        const bool draw_overlay = overlay_visible_ && (SDL_GetWindowFlags(SDL_GetWindowFromID(overlay_.id())) &
                                                       (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED)) == 0;
        if (draw_overlay && frame_started >= next_overlay_frame_) {
            const auto render_started = Clock::now();
            overlay_.prepare(state, player_card);
            overlay_.render(state, runtime_error_, &current_settings_, nullptr, nullptr, nullptr, nullptr);
            next_overlay_frame_ = render_started + std::chrono::milliseconds(overlay_.refresh_delay_ms());
        }
        ++frames_;
        if (frame_limit_ != 0 && frames_ >= frame_limit_)
            running_ = false;
        return running_;
    }

    [[nodiscard]] std::uint32_t next_wake_timeout_ms() const {
        if (!ready_ || !running_)
            return 0;
        auto next_wake = Clock::now() + poll_interval;
        if (runtime_ != nullptr)
            next_wake = std::min(next_wake, next_poll_);
        const auto tracker_flags = SDL_GetWindowFlags(SDL_GetWindowFromID(tracker_.id()));
        if ((tracker_flags & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED)) == 0)
            next_wake = std::min(next_wake, next_tracker_frame_);
        const bool draw_overlay = overlay_visible_ && (SDL_GetWindowFlags(SDL_GetWindowFromID(overlay_.id())) &
                                                       (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED)) == 0;
        if (draw_overlay)
            next_wake = std::min(next_wake, next_overlay_frame_);
        const auto remaining = next_wake - Clock::now();
        if (remaining <= Clock::duration::zero())
            return 0;
        return static_cast<std::uint32_t>(std::chrono::ceil<std::chrono::milliseconds>(remaining).count());
    }

private:
    void start_update_check() {
        const auto updater = update::layout::updater_executable(install_root_);
        const auto manifest = install_root_ / update::layout::current_pointer;
        std::error_code ec;
        if (!std::filesystem::is_regular_file(updater, ec) || ec)
            return;
        ec.clear();
        if (!std::filesystem::is_regular_file(manifest, ec) || ec)
            return;
        update_program_ = updater;
        update_check_ = start_process(
            {path_to_utf8(updater), "--check", "--machine-check", "--root", path_to_utf8(install_root_)}, true);
        if (update_check_ == nullptr)
            SDL_Log("Startup update check could not start: %s", SDL_GetError());
    }

    void poll_update_check() {
        int exit_code = 0;
        if (update_check_ == nullptr || !SDL_WaitProcess(update_check_, false, &exit_code))
            return;
        std::size_t size = 0;
        auto* output = static_cast<char*>(SDL_ReadProcess(update_check_, &size, nullptr));
        SDL_DestroyProcess(update_check_);
        update_check_ = nullptr;
        if (output == nullptr) {
            SDL_Log("Startup update check failed: %s", SDL_GetError());
            return;
        }
        std::string response(output, size);
        SDL_free(output);
        if (exit_code != 0) {
            SDL_Log("Startup update check failed with exit code %d", exit_code);
            return;
        }
        trim_line_end(response);
        constexpr std::string_view available = "BEACON_UPDATE_AVAILABLE\t";
        constexpr std::string_view current = "BEACON_UPDATE_CURRENT\t";
        const auto set_version = [&](const std::string_view prefix, const bool is_available) {
            if (!response.starts_with(prefix))
                return false;
            const auto version = std::string_view(response).substr(prefix.size());
            if (!update::parse_version(version))
                return false;
            if (is_available) {
                update_notice_.version = version;
                update_notice_.error.clear();
                next_tracker_frame_ = Clock::now();
            }
            return true;
        };
        if (!set_version(available, true) && !set_version(current, false))
            SDL_Log("Startup update check returned an invalid result");
    }

    void start_update_apply() {
        if (update_notice_.version.empty())
            return;
        const auto process =
            start_process({path_to_utf8(update_program_), "--apply", "--root", path_to_utf8(install_root_),
                           "--wait-for-parent", std::to_string(current_process_id())},
                          false);
        if (process == nullptr) {
            update_notice_.error = std::string("无法启动更新器：") + SDL_GetError();
            SDL_Log("Could not start updater: %s", SDL_GetError());
            next_tracker_frame_ = Clock::now();
            return;
        }
        SDL_DestroyProcess(process);
        running_ = false;
    }

    Runtime* runtime_ = nullptr;
    Persistence* persistence_ = nullptr;
    std::shared_ptr<const PublishedState> fixed_state_;
    std::filesystem::path asset_root_;
    std::filesystem::path install_root_;
    std::filesystem::path data_root_;
    const std::vector<std::filesystem::path>& template_options_;
    Settings current_settings_;
    Settings applied_settings_;
    int frame_limit_ = 0;
    int frames_ = 0;
    bool running_ = true;
    bool ready_ = false;
    bool overlay_visible_ = false;
    std::optional<Error> runtime_error_;
    std::optional<Error> settings_error_;
    bool apply_settings_ = false;
    SDL_Process* update_check_ = nullptr;
    std::filesystem::path update_program_;
    UpdateNotice update_notice_;
    ProgressViewModel view_;
    WindowRenderer tracker_;
    WindowRenderer overlay_;
    ProfileLoader profile_loader_;
    TemplateResourceLoader resource_loader_;
    Clock::time_point next_poll_{};
    Clock::time_point next_tracker_frame_{};
    Clock::time_point next_overlay_frame_{};
    std::shared_ptr<const PublishedState> previous_state_;
    std::shared_ptr<const PlayerCard> previous_player_;
};

DesktopLoop::DesktopLoop(Runtime* runtime, Persistence* persistence, std::shared_ptr<const PublishedState> fixed_state,
                         const Settings& settings, const std::filesystem::path& asset_root,
                         const std::filesystem::path& install_root, const std::filesystem::path& data_root,
                         const std::vector<std::filesystem::path>& templates, const int frame_limit,
                         std::optional<Error> initial_error)
    : impl_(std::make_unique<Impl>(runtime, persistence, std::move(fixed_state), settings, asset_root, install_root,
                                   data_root, templates, frame_limit, std::move(initial_error))) {}

DesktopLoop::~DesktopLoop() = default;

bool DesktopLoop::ready() const {
    return impl_->ready();
}

bool DesktopLoop::running() const {
    return impl_->running();
}

std::uint32_t DesktopLoop::next_wake_timeout_ms() const {
    return impl_->next_wake_timeout_ms();
}

void DesktopLoop::process_event(const SDL_Event& event) {
    impl_->process_event(event);
}

bool DesktopLoop::iterate() {
    return impl_->iterate();
}

namespace {

int run_loop(Runtime* runtime, Persistence* persistence, const std::shared_ptr<const PublishedState>& fixed_state,
             const std::filesystem::path& asset_root, const std::filesystem::path& install_root,
             const std::filesystem::path& data_root, const Settings& settings,
             const std::vector<std::filesystem::path>& template_options, int frame_limit,
             std::optional<Error> initial_error = std::nullopt) {
    constexpr int max_events_per_frame = 128;

    SdlSession session;
    if (!session.ready()) {
        SDL_Log("Initialization failed: %s", SDL_GetError());
        return 1;
    }
    DesktopLoop desktop(runtime, persistence, fixed_state, settings, asset_root, install_root, data_root,
                        template_options, frame_limit, std::move(initial_error));
    if (!desktop.ready()) {
        return 1;
    }
    std::optional<SDL_Event> pending_event;
    while (desktop.running()) {
        SDL_Event event{};
        for (int event_count = 0; event_count < max_events_per_frame && (pending_event || SDL_PollEvent(&event));
             ++event_count) {
            if (pending_event) {
                event = *pending_event;
                pending_event.reset();
            }
            desktop.process_event(event);
        }
        if (!desktop.running()) {
            break;
        }
        if (!desktop.iterate()) {
            break;
        }
        if (desktop.running() && SDL_WaitEventTimeout(&event, static_cast<Sint32>(desktop.next_wake_timeout_ms()))) {
            pending_event = event;
        }
    }

    return 0;
}

}  // namespace

int run_desktop(Runtime& runtime, Persistence& persistence, const Settings& settings,
                const std::filesystem::path& asset_root, const std::filesystem::path& install_root,
                const std::filesystem::path& data_root, const std::vector<std::filesystem::path>& templates,
                int frame_limit, std::optional<Error> initial_error) {
    return run_loop(&runtime, &persistence, {}, asset_root, install_root, data_root, settings, templates, frame_limit,
                    std::move(initial_error));
}

int desktop_smoke_test(const std::filesystem::path& asset_root) {
    constexpr std::string_view json =
        R"({"name_key":"template.smoke","minecraft":{"min_version":0,"max_version":99999999},"goals":[{"id":"goal","all":[{"id":"done","fact":"a","view":{"name_key":"done.smoke","icon":"minecraft/item/apple.png"}},{"id":"pending","fact":"b","view":{"name_key":"pending.smoke","icon":"minecraft/item/coal.png"}}],"view":{"name_key":"goal.smoke"}}],"completion_rule":"goal"})";
    auto compiled_value = compile_template_json(json);
    if (!compiled_value) {
        return 1;
    }
    if (!resolve_icon_paths(*compiled_value, asset_root, asset_root)) {
        return 1;
    }
    auto compiled = std::make_shared<const CompiledTemplate>(std::move(*compiled_value));
    auto localization = std::make_shared<const Localization>(Localization{.selected = {},
                                                                          .defaults = {{"template.smoke", "Smoke"},
                                                                                       {"goal.smoke", "Goal"},
                                                                                       {"done.smoke", "Done"},
                                                                                       {"pending.smoke", "Pending"}}});
    Layout layout_value;
    layout_value.main.push_back({"Progress", LayoutSource::Nodes, {"done", "pending"}, {}, 0, 0, 1, 1});
    layout_value.overlay.push_back({"Collections", LayoutSource::Children, {"goal"}, {}, 0, 0, 1, 0.7F});
    layout_value.overlay.push_back({"Stats", LayoutSource::Nodes, {}, {}, 0, 0.72F, 1, 0.28F});
    auto layout = std::make_shared<const Layout>(std::move(layout_value));
    Snapshot snapshot{
        .revision = 1,
        .run_epoch = 1,
        .results =
            {{.value = 1, .target = 2, .done = false, .active_child = 2},
             {.value = 1, .target = 1, .done = true, .active_child = std::numeric_limits<std::uint32_t>::max()},
             {.value = 0, .target = 1, .done = false, .active_child = std::numeric_limits<std::uint32_t>::max()}},
        .play_ticks = 20};
    auto state = std::make_shared<const PublishedState>(PublishedState{.run = {},
                                                                       .compiled = compiled,
                                                                       .snapshot = std::move(snapshot),
                                                                       .localization = localization,
                                                                       .layout = layout});
    SdlSession session;
    if (!session.ready())
        return 1;
    {
        ProgressViewModel view;
        view.update(*state);
        // Transparent windows are backend-specific; the smoke test only covers the
        // renderer and resource lifecycle shared by all desktop backends.
        WindowRenderer window("Refresh smoke", WindowSettings{}, true, asset_root, view, 1.0F, false);
        if (!window.ready())
            return 1;
        WindowRenderer tracker("Update notification smoke", WindowSettings{.width = 80, .height = 140}, false,
                               asset_root, view);
        if (!tracker.ready())
            return 1;
        UpdateNotice notice{.version = "1234567890.1234567890.1234567890"};
        Settings update_settings;
        update_settings.main_window_scale = max_window_scale;
        tracker.prepare({}, {});
        tracker.render({}, {}, &update_settings, nullptr, nullptr, nullptr, nullptr, &notice);
        int minimum_width = 0;
        int tracker_width = 0;
        SDL_GetWindowMinimumSize(SDL_GetWindowFromID(tracker.id()), &minimum_width, nullptr);
        SDL_GetWindowSize(SDL_GetWindowFromID(tracker.id()), &tracker_width, nullptr);
        if (minimum_width <= 80 || tracker_width < minimum_width) {
            SDL_Log("Update notification did not keep its button visible in a narrow window");
            return 1;
        }
        notice.error = "启动更新器失败，请重试";
        tracker.prepare(state, {});
        tracker.render(state, {}, &update_settings, nullptr, nullptr, nullptr, nullptr, &notice);
        window.prepare({}, {});
        window.render({}, {});
        if (window.refresh_delay_ms() != 1000) {
            SDL_Log("Expected idle refresh, got %llu", static_cast<unsigned long long>(window.refresh_delay_ms()));
            return 1;
        }
        SDL_SetRenderScale(window.renderer(), 2.0F, 2.0F);
        SDL_SetRenderDrawColor(window.renderer(), 12, 34, 56, 78);
        window.prepare(state, {});
        float scale_x = 0.0F;
        float scale_y = 0.0F;
        SDL_Color color{};
        SDL_GetRenderScale(window.renderer(), &scale_x, &scale_y);
        SDL_GetRenderDrawColor(window.renderer(), &color.r, &color.g, &color.b, &color.a);
        if (SDL_GetRenderTarget(window.renderer()) != nullptr || scale_x != 2.0F || scale_y != 2.0F || color.r != 12 ||
            color.g != 34 || color.b != 56 || color.a != 78) {
            SDL_Log("Icon atlas preparation did not preserve renderer state");
            return 1;
        }
        window.render(state, {});
        if (window.refresh_delay_ms() != 16) {
            SDL_Log("Expected active refresh, got %llu", static_cast<unsigned long long>(window.refresh_delay_ms()));
            return 1;
        }
        if (SDL_SetRenderVSync(window.renderer(), 0) && window.refresh_delay_ms() != 16) {
            SDL_Log("Expected timer fallback when VSync is disabled");
            return 1;
        }
        window.render({}, {});
        if (window.refresh_delay_ms() != 1000) {
            SDL_Log("Expected idle refresh, got %llu", static_cast<unsigned long long>(window.refresh_delay_ms()));
            return 1;
        }
        SDL_Event event{};
        event.type = SDL_EVENT_WINDOW_EXPOSED;
        event.window.windowID = window.id();
        window.process_event(event);
        if (window.refresh_delay_ms() != 16) {
            SDL_Log("Expected active refresh, got %llu", static_cast<unsigned long long>(window.refresh_delay_ms()));
            return 1;
        }
    }
    Settings settings;
    settings.overlay_visible = true;
    settings.overlay_transparent = false;
    return run_loop(nullptr, nullptr, state, asset_root, asset_root, asset_root, settings, {}, 1);
}

}  // namespace beacon
