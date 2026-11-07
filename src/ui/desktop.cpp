#include <beacon/ui/desktop.hpp>
#include <beacon/app/settings_applier.hpp>
#include <beacon/minecraft/process.hpp>
#include <beacon/ui/display.hpp>
#include <beacon/ui/language.hpp>
#include <beacon/ui/leaderboard.hpp>
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
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
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

bool window_shown(const SDL_WindowID id) {
    return (SDL_GetWindowFlags(SDL_GetWindowFromID(id)) & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED)) == 0;
}

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

// The startup update check: the updater's one-line answer on stdout and its exit code. SDL gives Windows no way to
// start a console program without a console window, so there the process is created directly.
class UpdateCheck {
public:
    UpdateCheck(const UpdateCheck&) = delete;
    UpdateCheck& operator=(const UpdateCheck&) = delete;

    struct Result {
        int exit_code = 0;
        std::string output;
    };

#if defined(_WIN32)
    static std::unique_ptr<UpdateCheck> start(const std::filesystem::path& updater, const std::filesystem::path& root) {
        SECURITY_ATTRIBUTES inherit{.nLength = sizeof(SECURITY_ATTRIBUTES), .bInheritHandle = TRUE};
        HANDLE read = nullptr;
        HANDLE write = nullptr;
        if (!CreatePipe(&read, &write, &inherit, 0)) {
            SDL_SetError("cannot create the updater pipe (error %lu)", GetLastError());
            return nullptr;
        }
        std::unique_ptr<UpdateCheck> check(new UpdateCheck);
        check->output_ = read;
        const HANDLE null = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                        &inherit, OPEN_EXISTING, 0, nullptr);
        STARTUPINFOW startup{.cb = sizeof(STARTUPINFOW), .dwFlags = STARTF_USESTDHANDLES};
        startup.hStdInput = null;
        startup.hStdOutput = write;
        startup.hStdError = null;
        auto command = quoted(updater.native()) + L" --check --machine-check --root " + quoted(root.native());
        PROCESS_INFORMATION process{};
        const bool started =
            SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0) &&
            CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                           CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr, &startup, &process);
        const auto error = GetLastError();
        CloseHandle(write);
        if (null != INVALID_HANDLE_VALUE)
            CloseHandle(null);
        if (!started) {
            SDL_SetError("cannot start the updater (error %lu)", error);
            return nullptr;
        }
        CloseHandle(process.hThread);
        check->process_ = process.hProcess;
        return check;
    }

    ~UpdateCheck() {
        if (WaitForSingleObject(process_, 0) != WAIT_OBJECT_0) {
            TerminateProcess(process_, 1);
            WaitForSingleObject(process_, INFINITE);
        }
        CloseHandle(process_);
        CloseHandle(output_);
    }

    // The result once the updater has exited; until then, drains its output so it never blocks on a full pipe.
    std::optional<Result> poll() {
        drain();
        DWORD exit_code = 0;
        if (WaitForSingleObject(process_, 0) != WAIT_OBJECT_0 || !GetExitCodeProcess(process_, &exit_code))
            return std::nullopt;
        drain();
        return Result{.exit_code = static_cast<int>(exit_code), .output = std::move(text_)};
    }

private:
    UpdateCheck() = default;

    // Quotes one argument the way CommandLineToArgvW splits it.
    static std::wstring quoted(const std::wstring& argument) {
        std::wstring result = L"\"";
        std::size_t backslashes = 0;
        for (const wchar_t c : argument) {
            if (c == L'\\') {
                ++backslashes;
                continue;
            }
            result.append(c == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
            backslashes = 0;
            result.push_back(c);
        }
        result.append(backslashes * 2, L'\\');
        result.push_back(L'"');
        return result;
    }

    void drain() {
        DWORD available = 0;
        while (PeekNamedPipe(output_, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
            char buffer[4096];
            DWORD read = 0;
            if (!ReadFile(output_, buffer, std::min<DWORD>(available, sizeof(buffer)), &read, nullptr) || read == 0)
                return;
            text_.append(buffer, read);
        }
    }

    HANDLE process_ = nullptr;
    HANDLE output_ = nullptr;
    std::string text_;
#else
    static std::unique_ptr<UpdateCheck> start(const std::filesystem::path& updater, const std::filesystem::path& root) {
        auto* process =
            start_process({path_to_utf8(updater), "--check", "--machine-check", "--root", path_to_utf8(root)}, true);
        if (process == nullptr)
            return nullptr;
        std::unique_ptr<UpdateCheck> check(new UpdateCheck);
        check->process_ = process;
        return check;
    }

    ~UpdateCheck() {
        if (!SDL_WaitProcess(process_, false, nullptr))
            SDL_KillProcess(process_, true);
        SDL_DestroyProcess(process_);
    }

    // The result once the updater has exited.
    std::optional<Result> poll() {
        int exit_code = 0;
        if (!SDL_WaitProcess(process_, false, &exit_code))
            return std::nullopt;
        std::size_t size = 0;
        auto* output = static_cast<char*>(SDL_ReadProcess(process_, &size, nullptr));
        if (output == nullptr)
            return Result{.exit_code = -1};
        Result result{.exit_code = exit_code, .output = std::string(output, size)};
        SDL_free(output);
        return result;
    }

private:
    UpdateCheck() = default;

    SDL_Process* process_ = nullptr;
#endif
};

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
         const std::vector<std::filesystem::path>& template_options, const int frame_limit,
         std::optional<Error> initial_error)
        : runtime_(runtime), persistence_(persistence), fixed_state_(std::move(fixed_state)), asset_root_(asset_root),
          install_root_(install_root), template_options_(template_options), current_settings_(settings),
          applied_settings_(settings), frame_limit_(frame_limit), runtime_error_(std::move(initial_error)),
          tracker_("Beacon Tracker",
                   WindowSettings{.width = settings.main_window_width, .height = settings.main_window_height}, false,
                   asset_root_, view_, settings.main_window_scale),
          overlay_(
              "Beacon Overlay",
              WindowSettings{
                  .x = 120, .y = 120, .width = settings.overlay_window_width, .height = settings.overlay_window_height},
              true, asset_root_, view_, settings.overlay_window_scale, settings.overlay_transparent,
              settings.overlay_visible),
          profile_loader_(install_root_), leaderboard_loaders_(install_root_) {
        if (runtime_ != nullptr && persistence_ != nullptr) {
            const auto initial_state = runtime_->state();
            settings_applier_.emplace(
                *runtime_, *persistence_, asset_root_,
                TemplateResourceLoader(
                    settings.template_path, asset_root_,
                    initial_state
                        ? TemplateResources{initial_state->compiled, initial_state->localization, initial_state->layout}
                        : TemplateResources{}));
        }
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
        save_window_sizes();
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
        if (event.type == SDL_EVENT_WINDOW_RESIZED)
            remember_window_size(event.window.windowID, frame_started);
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
        if (resize_save_at_ && frame_started >= *resize_save_at_)
            save_window_sizes();
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
                            &apply_settings_, runtime_, &update_notice_, &leaderboard_loaders_);
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
            auto applied =
                settings_applier_
                    ? settings_applier_->apply(applied_settings_, current_settings_)
                    : ylt::expected<void, Error>{ylt::unexpected<Error>{
                          {.code = ErrorCode::Internal, .message = "runtime is unavailable", .context = "settings"}}};
            if (!applied) {
                settings_error_ = std::move(applied.error());
            } else {
                const auto resize = [&](const SDL_WindowID id, const int width, const int height) {
                    auto* window = SDL_GetWindowFromID(id);
                    if ((SDL_GetWindowFlags(window) & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED)) != 0 &&
                        !SDL_RestoreWindow(window)) {
                        settings_error_ =
                            Error{.code = ErrorCode::Internal, .message = SDL_GetError(), .context = "window size"};
                        return;
                    }
                    int actual_width = 0;
                    int actual_height = 0;
                    SDL_GetWindowSize(window, &actual_width, &actual_height);
                    if ((actual_width != width || actual_height != height) &&
                        !SDL_SetWindowSize(window, width, height)) {
                        settings_error_ =
                            Error{.code = ErrorCode::Internal, .message = SDL_GetError(), .context = "window size"};
                    }
                };
                settings_error_.reset();
                if (current_settings_.main_window_width != applied_settings_.main_window_width ||
                    current_settings_.main_window_height != applied_settings_.main_window_height)
                    resize(tracker_.id(), current_settings_.main_window_width, current_settings_.main_window_height);
                if (current_settings_.overlay_window_width != applied_settings_.overlay_window_width ||
                    current_settings_.overlay_window_height != applied_settings_.overlay_window_height)
                    resize(overlay_.id(), current_settings_.overlay_window_width,
                           current_settings_.overlay_window_height);
                applied_settings_ = current_settings_;
                resize_save_at_.reset();
                runtime_error_.reset();
                next_tracker_frame_ = next_overlay_frame_ = frame_started;
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
        if (overlay_visible_ && window_shown(overlay_.id()) && frame_started >= next_overlay_frame_) {
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
        if (resize_save_at_)
            next_wake = std::min(next_wake, *resize_save_at_);
        if (runtime_ != nullptr)
            next_wake = std::min(next_wake, next_poll_);
        if (window_shown(tracker_.id()))
            next_wake = std::min(next_wake, next_tracker_frame_);
        if (overlay_visible_ && window_shown(overlay_.id()))
            next_wake = std::min(next_wake, next_overlay_frame_);
        const auto remaining = next_wake - Clock::now();
        if (remaining <= Clock::duration::zero())
            return 0;
        return static_cast<std::uint32_t>(std::chrono::ceil<std::chrono::milliseconds>(remaining).count());
    }

private:
    void remember_window_size(const SDL_WindowID id, const Clock::time_point now) {
        if (id != tracker_.id() && id != overlay_.id())
            return;
        auto* window = SDL_GetWindowFromID(id);
        if ((SDL_GetWindowFlags(window) & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED)) != 0)
            return;
        int width = 0;
        int height = 0;
        if (!SDL_GetWindowSize(window, &width, &height) || width < 1 || height < 1 || width > max_window_dimension ||
            height > max_window_dimension)
            return;
        const auto remember = [&](const int actual, int& applied, int& draft) {
            if (actual == applied)
                return;
            if (draft == applied)
                draft = actual;
            applied = actual;
            resize_save_at_ = now + std::chrono::milliseconds(500);
        };
        if (id == tracker_.id()) {
            remember(width, applied_settings_.main_window_width, current_settings_.main_window_width);
            remember(height, applied_settings_.main_window_height, current_settings_.main_window_height);
        } else {
            remember(width, applied_settings_.overlay_window_width, current_settings_.overlay_window_width);
            remember(height, applied_settings_.overlay_window_height, current_settings_.overlay_window_height);
        }
    }

    void save_window_sizes() {
        if (persistence_ == nullptr || !resize_save_at_)
            return;
        if (auto result = persistence_->save_settings(applied_settings_); !result) {
            settings_error_ = std::move(result.error());
            settings_error_->message += ": " + settings_error_->context;
            settings_error_->context = "window sizes";
            SDL_Log("Could not save window sizes: %s", settings_error_->message.c_str());
            next_tracker_frame_ = Clock::now();
            resize_save_at_ = next_tracker_frame_ + std::chrono::seconds(1);
            return;
        }
        resize_save_at_.reset();
        if (settings_error_ && settings_error_->context == "window sizes")
            settings_error_.reset();
    }

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
        update_check_ = UpdateCheck::start(updater, install_root_);
        if (update_check_ == nullptr)
            SDL_Log("Startup update check could not start: %s", SDL_GetError());
    }

    void poll_update_check() {
        auto result = update_check_ != nullptr ? update_check_->poll() : std::nullopt;
        if (!result)
            return;
        update_check_.reset();
        if (result->exit_code != 0) {
            SDL_Log("Startup update check failed with exit code %d", result->exit_code);
            return;
        }
        auto& response = result->output;
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
            update_notice_.error = std::string(interface_text(current_settings_.ui_language, "无法启动更新器：",
                                                              "Could not start the updater: ")) +
                                   SDL_GetError();
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
    // Owned: callers may pass a temporary, such as the defaulted empty list.
    std::vector<std::filesystem::path> template_options_;

    Settings current_settings_;
    Settings applied_settings_;
    std::optional<SettingsApplier> settings_applier_;
    std::optional<Error> settings_error_;
    bool apply_settings_ = false;
    std::optional<Clock::time_point> resize_save_at_;

    int frame_limit_ = 0;
    int frames_ = 0;
    bool running_ = true;
    bool ready_ = false;
    bool overlay_visible_ = false;
    std::optional<Error> runtime_error_;

    std::unique_ptr<UpdateCheck> update_check_;
    std::filesystem::path update_program_;
    UpdateNotice update_notice_;

    ProgressViewModel view_;
    WindowRenderer tracker_;
    WindowRenderer overlay_;
    ProfileLoader profile_loader_;
    LeaderboardLoaders leaderboard_loaders_;

    Clock::time_point next_poll_{};
    Clock::time_point next_tracker_frame_{};
    Clock::time_point next_overlay_frame_{};
    std::shared_ptr<const PublishedState> previous_state_;
    std::shared_ptr<const PlayerCard> previous_player_;
};

DesktopLoop::DesktopLoop(Runtime* runtime, Persistence* persistence, std::shared_ptr<const PublishedState> fixed_state,
                         const Settings& settings, const std::filesystem::path& asset_root,
                         const std::filesystem::path& install_root, const std::vector<std::filesystem::path>& templates,
                         const int frame_limit, std::optional<Error> initial_error)
    : impl_(std::make_unique<Impl>(runtime, persistence, std::move(fixed_state), settings, asset_root, install_root,
                                   templates, frame_limit, std::move(initial_error))) {}

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
             const Settings& settings, const std::vector<std::filesystem::path>& template_options, int frame_limit,
             std::optional<Error> initial_error = std::nullopt) {
    constexpr int max_events_per_frame = 128;

    SdlSession session;
    if (!session.ready()) {
        SDL_Log("Initialization failed: %s", SDL_GetError());
        return 1;
    }
    DesktopLoop desktop(runtime, persistence, fixed_state, settings, asset_root, install_root, template_options,
                        frame_limit, std::move(initial_error));
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
                const std::vector<std::filesystem::path>& templates, int frame_limit,
                std::optional<Error> initial_error) {
    return run_loop(&runtime, &persistence, {}, asset_root, install_root, settings, templates, frame_limit,
                    std::move(initial_error));
}

int desktop_smoke_test(const std::filesystem::path& asset_root) {
    constexpr std::string_view json =
        R"({"name_key":"template.smoke","minecraft":{"min_version":0,"max_version":99999999},"goals":[
          {"id":"goal","all":[
            {"id":"done","fact":"a","view":{"name_key":"done.smoke"}},
            {"id":"pending","fact":"b","view":{"name_key":"pending.smoke"}}
          ],"view":{"name_key":"goal.smoke"}},
          {"id":"counter","prefix":[
            {"fact":"stat/first","target":1,"view":{"name_key":"done.smoke","type":"task"}},
            {"fact":"stat/last","target":1,"view":{"name_key":"pending.smoke","type":"task"}}
          ]}
        ],"completion_rule":"goal"})";
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
    auto layout_value = compile_layout_json(R"({
          "main": {"vbox": [
            {"id": "Progress", "stretch": 70, "nodes": ["done", "pending"]},
            {"spring": true, "stretch": 2},
            {"id": "Stats", "stretch": 28, "nodes": ["counter"]}
          ]},
          "overlay": {"vbox": [
            {"id": "Collections", "stretch": 70, "collections": ["goal"]},
            {"spring": true, "stretch": 2},
            {"id": "Stats", "stretch": 28, "nodes": ["counter"]}
          ]}
        })",
                                            *compiled);
    if (!layout_value)
        return 1;
    auto layout = std::make_shared<const Layout>(std::move(*layout_value));
    Snapshot snapshot{
        .revision = 1, .run_epoch = 1, .results = evaluate(*compiled, {{"a", 1}, {"stat/first", 1}}), .play_ticks = 20};
    auto state = std::make_shared<const PublishedState>(PublishedState{
        .compiled = compiled, .localization = localization, .layout = layout, .snapshot = std::move(snapshot)});
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
        int tracker_width = 0;
        int tracker_height = 0;
        SDL_GetWindowSize(SDL_GetWindowFromID(tracker.id()), &tracker_width, &tracker_height);
        if (tracker_width != 80 || tracker_height != 140) {
            SDL_Log("Update notification changed the configured window size");
            return 1;
        }
        notice.error = "启动更新器失败，请重试";
        tracker.prepare(state, {});
        tracker.render(state, {}, &update_settings, nullptr, nullptr, nullptr, nullptr, &notice);
        SDL_GetWindowSize(SDL_GetWindowFromID(tracker.id()), &tracker_width, &tracker_height);
        if (tracker_width != 80 || tracker_height != 140) {
            SDL_Log("Layout changed the configured window size");
            return 1;
        }
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
        int overlay_width = 0;
        int overlay_height = 0;
        SDL_GetWindowSize(SDL_GetWindowFromID(window.id()), &overlay_width, &overlay_height);
        if (overlay_width != 960 || overlay_height != 640) {
            SDL_Log("Layout changed the configured overlay size");
            return 1;
        }
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
    return run_loop(nullptr, nullptr, state, asset_root, asset_root, settings, {}, 1);
}

}  // namespace beacon
