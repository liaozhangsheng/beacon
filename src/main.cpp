#include <beacon/ui/desktop.hpp>
#include <beacon/ui/display.hpp>
#include <beacon/io/file.hpp>

#include <SDL3/SDL.h>
#if defined(_WIN32)
    #define SDL_MAIN_USE_CALLBACKS
    #include <SDL3/SDL_main.h>
#endif

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace {

int fail(const beacon::Error& error) {
    std::cerr << error.message << ": " << error.context << '\n';
    return 1;
}

std::filesystem::path default_game_root() {
#if defined(_WIN32)
    if (const auto* root = _wgetenv(L"APPDATA")) {
        return std::filesystem::path(root) / L".minecraft";
    }
#elif defined(__APPLE__)
    if (const auto* root = std::getenv("HOME")) {
        return beacon::path_from_utf8(root) / "Library/Application Support/minecraft";
    }
#else
    if (const auto* root = std::getenv("HOME")) {
        return beacon::path_from_utf8(root) / ".minecraft";
    }
#endif
    std::error_code ec;
    const auto current = std::filesystem::current_path(ec);
    return (ec ? std::filesystem::path{} : current) / beacon::path_from_utf8(".minecraft");
}

std::filesystem::path path_under_templates(const std::filesystem::path& path) {
    const auto normalized = path.lexically_normal();
    auto marker = std::find(normalized.begin(), normalized.end(), std::filesystem::path("templates"));
    if (marker == normalized.end()) {
        return {};
    }
    std::filesystem::path relative;
    for (++marker; marker != normalized.end(); ++marker) {
        relative /= *marker;
    }
    return relative;
}

ylt::expected<std::vector<std::filesystem::path>, beacon::Error> find_templates(const std::filesystem::path& root) {
    std::vector<std::filesystem::path> result;
    const auto scan_error = [] {
        return ylt::unexpected<beacon::Error>{
            {.code = beacon::ErrorCode::Io, .message = "cannot scan templates directory", .context = "templates"}};
    };
    std::error_code ec;
    std::filesystem::directory_iterator entries(root / "templates", ec);
    if (ec) {
        return scan_error();
    }
    for (; entries != std::filesystem::directory_iterator{}; entries.increment(ec)) {
        if (ec) {
            return scan_error();
        }
        if (!entries->is_directory(ec)) {
            if (ec) {
                return ylt::unexpected<beacon::Error>{{beacon::ErrorCode::Io, "cannot inspect template directory",
                                                       beacon::path_to_utf8(entries->path().filename())}};
            }
            continue;
        }
        const auto file = entries->path() / "template.json";
        ec.clear();
        if (std::filesystem::is_regular_file(file, ec) && !ec) {
            if (auto canonical = std::filesystem::weakly_canonical(file, ec); !ec) {
                result.push_back(std::move(canonical));
            }
        }
    }
    if (ec) {
        return scan_error();
    }
    std::sort(result.begin(), result.end());
    if (result.empty()) {
        return ylt::unexpected<beacon::Error>{{beacon::ErrorCode::Io, "no templates found", "templates"}};
    }
    return result;
}

struct Application {
    std::vector<std::filesystem::path> templates;
    std::unique_ptr<beacon::Persistence> persistence;
    std::unique_ptr<beacon::Runtime> runtime;
    std::unique_ptr<beacon::DesktopLoop> desktop;
    beacon::Settings settings;
    std::filesystem::path asset_root;
    std::filesystem::path data_root;
    std::optional<beacon::Error> startup_error;
};

int run_smoke_test() {
    auto* executable_root = SDL_GetBasePath();
    if (executable_root == nullptr) {
        std::cerr << "cannot locate executable directory: " << SDL_GetError() << '\n';
        return 1;
    }
    const auto asset_root = beacon::path_from_utf8(executable_root);
    return beacon::desktop_smoke_test(asset_root);
}

std::unique_ptr<Application> create_application(const int argc, int& status) {
    status = 1;
    auto* executable_root = SDL_GetBasePath();
    if (executable_root == nullptr) {
        std::cerr << "cannot locate executable directory: " << SDL_GetError() << '\n';
        return {};
    }
    const auto asset_root = beacon::path_from_utf8(executable_root);
    if (argc != 1) {
        std::cerr << "usage: beacon\n";
        status = 2;
        return {};
    }

    auto templates = find_templates(asset_root);
    if (!templates) {
        status = fail(templates.error());
        return {};
    }
    auto* preference_root = SDL_GetPrefPath("DHU-Minecraft", "Beacon");
    if (preference_root == nullptr) {
        std::cerr << "cannot locate writable data directory: " << SDL_GetError() << '\n';
        return {};
    }
    const auto data_root = beacon::path_from_utf8(preference_root);
    SDL_free(preference_root);

    auto application = std::make_unique<Application>();
    application->templates = std::move(*templates);
    application->persistence = std::make_unique<beacon::Persistence>(asset_root);
    std::optional<beacon::Error> startup_error;
    const auto remember_error = [&](beacon::Error error) {
        if (!startup_error)
            startup_error = std::move(error);
    };
    auto settings = beacon::Settings{.game_root = default_game_root(), .template_path = application->templates.front()};
    auto loaded = application->persistence->load_settings();
    if (!loaded) {
        remember_error(std::move(loaded.error()));
    } else if (!*loaded) {
        if (auto saved = application->persistence->save_settings(settings); !saved)
            remember_error(std::move(saved.error()));
    } else {
        settings = std::move(**loaded);
    }

    std::error_code ec;
    const auto configured_template = std::filesystem::weakly_canonical(settings.template_path, ec);
    const auto configured_relative = path_under_templates(settings.template_path);
    auto template_file =
        std::find_if(application->templates.begin(), application->templates.end(), [&](const auto& candidate) {
            return (!ec && candidate == configured_template) ||
                   (!configured_relative.empty() && path_under_templates(candidate) == configured_relative);
        });
    if (template_file == application->templates.end()) {
        remember_error({.code = beacon::ErrorCode::Validation,
                        .message = "模板配置无效，已使用默认模板",
                        .context = "template_path"});
        template_file = application->templates.begin();
    }
    const auto load_resources = [&](const auto file) {
        settings.template_path = *file;
        if (!settings.language.empty()) {
            const auto languages = beacon::available_template_languages(*file);
            if (std::find(languages.begin(), languages.end(), settings.language) == languages.end()) {
                remember_error({.code = beacon::ErrorCode::Validation,
                                .message = "语言配置无效，已使用默认语言",
                                .context = "language"});
                settings.language.clear();
            }
        }
        return beacon::load_template_resources(*file, asset_root, settings.language);
    };
    auto resources = load_resources(template_file);
    if (!resources && template_file != application->templates.begin()) {
        remember_error(std::move(resources.error()));
        template_file = application->templates.begin();
        resources = load_resources(template_file);
    }
    if (!resources) {
        status = fail(resources.error());
        return {};
    }

    application->runtime = std::make_unique<beacon::Runtime>();
    if (auto selected = application->runtime->configure(settings.game_root, resources->compiled,
                                                        resources->localization, resources->layout);
        !selected) {
        status = fail(selected.error());
        return {};
    }
    application->settings = std::move(settings);
    application->asset_root = asset_root;
    application->data_root = data_root;
    application->startup_error = std::move(startup_error);
    status = 0;
    return application;
}

}  // namespace

#if defined(_WIN32)

SDL_AppResult SDLCALL SDL_AppInit(void** appstate, const int argc, char** argv) {
    *appstate = nullptr;
    SDL_SetHint(SDL_HINT_MAIN_CALLBACK_RATE, "60");
    if (argc == 2 && std::string_view(argv[1]) == "--smoke-test")
        return run_smoke_test() == 0 ? SDL_APP_SUCCESS : SDL_APP_FAILURE;

    int status = 1;
    auto application = create_application(argc, status);
    if (!application)
        return SDL_APP_FAILURE;
    application->desktop =
        std::make_unique<beacon::DesktopLoop>(application->runtime.get(), application->persistence.get(), nullptr,
                                              application->settings, application->asset_root, application->data_root,
                                              application->templates, 0, std::move(application->startup_error));
    if (!application->desktop->ready())
        return SDL_APP_FAILURE;
    *appstate = application.release();
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDLCALL SDL_AppIterate(void* appstate) {
    auto* application = static_cast<Application*>(appstate);
    return application != nullptr && application->desktop->iterate() ? SDL_APP_CONTINUE : SDL_APP_SUCCESS;
}

SDL_AppResult SDLCALL SDL_AppEvent(void* appstate, SDL_Event* event) {
    auto* application = static_cast<Application*>(appstate);
    if (application == nullptr)
        return SDL_APP_FAILURE;
    application->desktop->process_event(*event);
    return application->desktop->running() ? SDL_APP_CONTINUE : SDL_APP_SUCCESS;
}

void SDLCALL SDL_AppQuit(void* appstate, SDL_AppResult) {
    delete static_cast<Application*>(appstate);
}

#else

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--smoke-test") {
        return run_smoke_test();
    }
    int status = 1;
    auto application = create_application(argc, status);
    if (!application)
        return status;
    return beacon::run_desktop(*application->runtime, *application->persistence, application->settings,
                               application->asset_root, application->data_root, application->templates, 0,
                               std::move(application->startup_error));
}

#endif
