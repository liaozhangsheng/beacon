#include "temporary_directory.hpp"
#include "../src/ui/desktop_renderer.hpp"

#include <beacon/ui/desktop.hpp>
#include <beacon/ui/progress.hpp>
#include <beacon/io/file.hpp>
#include <SDL3/SDL.h>
#include <imgui_internal.h>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string_view>

namespace {

struct VideoSession {
    VideoSession() {
        const auto* driver = SDL_getenv("BEACON_TEST_VIDEO_DRIVER");
        REQUIRE(SDL_SetHint(SDL_HINT_VIDEO_DRIVER, driver ? driver : "dummy"));
        REQUIRE(SDL_InitSubSystem(SDL_INIT_VIDEO));
    }
    ~VideoSession() {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }
};

SDL_Window* find_window(const std::string_view title) {
    int count = 0;
    auto** windows = SDL_GetWindows(&count);
    SDL_Window* result = nullptr;
    for (int index = 0; index < count; ++index) {
        if (SDL_GetWindowTitle(windows[index]) == title)
            result = windows[index];
    }
    SDL_free(windows);
    return result;
}

std::array<int, 2> window_size(SDL_Window* window) {
    std::array<int, 2> result{};
    REQUIRE(SDL_GetWindowSize(window, &result[0], &result[1]));
    return result;
}

void resize(beacon::DesktopLoop& loop, SDL_Window* window, const int width, const int height) {
    REQUIRE(SDL_SetWindowSize(window, width, height));
    SDL_Event event{};
    event.type = SDL_EVENT_WINDOW_RESIZED;
    event.window.windowID = SDL_GetWindowID(window);
    loop.process_event(event);
}

beacon::Settings load(const beacon::Persistence& persistence) {
    const auto loaded = persistence.load_settings();
    REQUIRE(loaded);
    REQUIRE(*loaded);
    return **loaded;
}

}  // namespace

TEST_CASE("saving dimension inputs resizes both windows and rejects invalid sizes") {
    const VideoSession video;
    const beacon::test::TemporaryDirectory temporary;
    beacon::Persistence persistence(temporary.path);
    beacon::Settings settings{.game_root = temporary.path, .template_path = temporary.path / "template.json"};
    settings.auto_detect = false;
    settings.overlay_visible = false;
    REQUIRE(persistence.save_settings(settings));
    beacon::Runtime runtime;
    beacon::DesktopLoop loop(&runtime, &persistence, {}, settings, BEACON_TEST_ROOT, temporary.path);
    REQUIRE(loop.ready());
    auto* tracker = find_window("Beacon Tracker");
    auto* overlay = find_window("Beacon Overlay");
    REQUIRE(tracker);
    REQUIRE(overlay);
    const auto frame = [&] {
        SDL_Event event{};
        event.type = SDL_EVENT_WINDOW_EXPOSED;
        event.window.windowID = SDL_GetWindowID(tracker);
        loop.process_event(event);
        SDL_Delay(40);
        REQUIRE(loop.iterate());
    };
    frame();
    auto* main = ImGui::FindWindowByName("Tracker##beacon");
    REQUIRE(main);
    ImGui::ActivateItemByID(main->GetID("##settings"));
    frame();
    frame();
    auto* book = ImGui::FindWindowByName("###beacon-settings");
    REQUIRE(book);
    REQUIRE(book->DC.ChildWindows.Size == 1);
    auto* body = book->DC.ChildWindows[0];
    const auto edit = [&](const char* label, const int component, const char* value) {
        const auto group_id = ImHashStr(label, 0, body->GetID("##appearance-settings"));
        const auto input_id = ImHashStr("", 0, ImHashData(&component, sizeof(component), group_id));
        ImGui::ActivateItemByID(input_id);
        frame();
        auto* input = ImGui::GetInputTextState(input_id);
        REQUIRE(input);
        input->SelectAll();
        ImGui::GetIO().AddInputCharactersUTF8(value);
        frame();
    };
    edit("##main-window-size", 0, "-1");
    ImGui::ActivateItemByID(book->GetID("##save"));
    frame();
    CHECK(load(persistence).main_window_width == 960);
    CHECK(window_size(tracker) == std::array{960, 640});
    edit("##main-window-size", 0, "1080");
    ImGui::ClearActiveID();
    frame();
    resize(loop, tracker, 1000, 700);
    SDL_Delay(550);
    frame();
    CHECK(load(persistence).main_window_width == 1000);
    CHECK(load(persistence).main_window_height == 700);
    edit("##main-window-size", 1, "720");
    body->Scroll.y = body->ScrollMax.y;
    frame();
    edit("##overlay-window-size", 0, "1200");
    ImGui::ClearActiveID();
    frame();
    // Layout growth must not replace the edited width with the window's current width.
    resize(loop, overlay, 1280, 300);
    frame();
    edit("##overlay-window-size", 1, "240");
    ImGui::ActivateItemByID(book->GetID("##save"));
    frame();
    CHECK(window_size(tracker) == std::array{1080, 720});
    CHECK(window_size(overlay) == std::array{1200, 240});
    CHECK((SDL_GetWindowFlags(overlay) & SDL_WINDOW_HIDDEN) != 0);
    const auto saved = load(persistence);
    CHECK(saved.main_window_width == 1080);
    CHECK(saved.main_window_height == 720);
    CHECK(saved.overlay_window_width == 1200);
    CHECK(saved.overlay_window_height == 240);
    // The user's saved height remains subject to the layout's minimum height.
    REQUIRE(SDL_SetWindowMinimumSize(overlay, 0, 300));
    REQUIRE(SDL_SetWindowSize(overlay, 1200, 300));
    edit("##overlay-window-size", 1, "1");
    ImGui::ActivateItemByID(book->GetID("##save"));
    frame();
    CHECK(window_size(overlay) == std::array{1200, 300});
    SDL_Delay(550);
    frame();
    CHECK(load(persistence).overlay_window_height == 1);
}

TEST_CASE("desktop sizes debounce both windows and preserve uncommitted settings") {
    const VideoSession video;
    const beacon::test::TemporaryDirectory temporary;
    beacon::Persistence persistence(temporary.path);
    beacon::Settings settings{.game_root = temporary.path, .template_path = temporary.path / "template.json"};
    REQUIRE(persistence.save_settings(settings));
    {
        beacon::DesktopLoop loop(nullptr, &persistence, {}, settings, BEACON_TEST_ROOT, temporary.path);
        REQUIRE(loop.ready());
        auto* tracker = find_window("Beacon Tracker");
        auto* overlay = find_window("Beacon Overlay");
        REQUIRE(tracker);
        REQUIRE(overlay);
        CHECK(window_size(tracker) == std::array{960, 640});
        CHECK(window_size(overlay) == std::array{1280, 180});
        // Closing the overlay changes the draft setting; a resize must not save that edit.
        SDL_Event close{};
        close.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
        close.window.windowID = SDL_GetWindowID(overlay);
        loop.process_event(close);
        resize(loop, tracker, 1000, 700);
        REQUIRE(loop.iterate());
        CHECK(load(persistence).main_window_width == 960);
        SDL_Delay(300);
        resize(loop, tracker, 1100, 750);
        resize(loop, overlay, 1200, 200);
        REQUIRE(loop.iterate());
        SDL_Delay(300);
        REQUIRE(loop.iterate());
        CHECK(load(persistence).main_window_width == 960);
        SDL_Delay(250);
        REQUIRE(loop.iterate());
        const auto saved = load(persistence);
        CHECK(saved.main_window_width == 1100);
        CHECK(saved.main_window_height == 750);
        CHECK(saved.overlay_window_width == 1200);
        CHECK(saved.overlay_window_height == 200);
        CHECK(saved.overlay_visible);
        CHECK((SDL_GetWindowFlags(overlay) & SDL_WINDOW_HIDDEN) != 0);
        // Programmatic notifications of the saved size must not write another backup.
        const auto backup = beacon::read_bounded_file(temporary.path / "config/settings.json.bak", "backup");
        REQUIRE(backup);
        resize(loop, tracker, 1100, 750);
        SDL_Delay(550);
        REQUIRE(loop.iterate());
        const auto unchanged = beacon::read_bounded_file(temporary.path / "config/settings.json.bak", "backup");
        REQUIRE(unchanged);
        CHECK(*unchanged == *backup);
        // Pending sizes are flushed on exit, even without another frame.
        resize(loop, tracker, 1150, 760);
    }
    settings = load(persistence);
    CHECK(settings.main_window_width == 1150);
    CHECK(settings.main_window_height == 760);
    beacon::DesktopLoop restored(nullptr, &persistence, {}, settings, BEACON_TEST_ROOT, temporary.path);
    REQUIRE(restored.ready());
    CHECK(window_size(find_window("Beacon Tracker")) == std::array{1150, 760});
    CHECK(window_size(find_window("Beacon Overlay")) == std::array{1200, 200});
}

TEST_CASE("desktop size saves retry after storage recovers") {
    const VideoSession video;
    const beacon::test::TemporaryDirectory temporary;
    beacon::Persistence persistence(temporary.path);
    beacon::Settings settings{.game_root = temporary.path, .template_path = temporary.path / "template.json"};
    REQUIRE(persistence.save_settings(settings));
    beacon::DesktopLoop loop(nullptr, &persistence, {}, settings, BEACON_TEST_ROOT, temporary.path);
    REQUIRE(loop.ready());
    const auto backup = temporary.path / "config/settings.json.bak";
    REQUIRE(std::filesystem::create_directory(backup));
    resize(loop, find_window("Beacon Tracker"), 1000, 700);
    SDL_Delay(550);
    REQUIRE(loop.iterate());
    CHECK(load(persistence).main_window_width == 960);
    REQUIRE(std::filesystem::remove(backup));
    SDL_Delay(1100);
    REQUIRE(loop.iterate());
    CHECK(load(persistence).main_window_width == 1000);
    CHECK(load(persistence).main_window_height == 700);
}

TEST_CASE("maximized and minimized desktop windows preserve normal sizes") {
    const VideoSession video;
    const beacon::test::TemporaryDirectory temporary;
    beacon::Persistence persistence(temporary.path);
    beacon::Settings settings{.game_root = temporary.path, .template_path = temporary.path / "template.json"};
    REQUIRE(persistence.save_settings(settings));
    {
        beacon::DesktopLoop loop(nullptr, &persistence, {}, settings, BEACON_TEST_ROOT, temporary.path);
        REQUIRE(loop.ready());
        auto* tracker = find_window("Beacon Tracker");
        REQUIRE(tracker);
        if (!SDL_MaximizeWindow(tracker))
            SKIP("The dummy video driver does not support maximizing windows.");
        REQUIRE(SDL_SyncWindow(tracker));
        REQUIRE((SDL_GetWindowFlags(tracker) & SDL_WINDOW_MAXIMIZED) != 0);
        resize(loop, tracker, 1500, 900);
        REQUIRE(SDL_RestoreWindow(tracker));
        REQUIRE(SDL_SyncWindow(tracker));
        REQUIRE(SDL_MinimizeWindow(tracker));
        REQUIRE(SDL_SyncWindow(tracker));
        REQUIRE((SDL_GetWindowFlags(tracker) & SDL_WINDOW_MINIMIZED) != 0);
        resize(loop, tracker, 800, 500);
    }
    const auto saved = load(persistence);
    CHECK(saved.main_window_width == 960);
    CHECK(saved.main_window_height == 640);
}

TEST_CASE("hidden overlays preserve dimensions when transparency recreates their window") {
    const VideoSession video;
    beacon::ProgressViewModel view;
    beacon::WindowRenderer overlay("Overlay", {.width = 1234, .height = 234}, true, BEACON_TEST_ROOT, view, 1.0F, false,
                                   false);
    REQUIRE(overlay.ready());
    const auto original_id = overlay.id();
    if (overlay.set_overlay_transparent(true)) {
        CHECK(overlay.id() != original_id);
        CHECK(window_size(SDL_GetWindowFromID(overlay.id())) == std::array{1234, 234});
        REQUIRE(overlay.set_overlay_transparent(false));
    } else {
        INFO("The dummy driver does not support transparency; failed recreation must preserve the original.");
        CHECK(overlay.id() == original_id);
    }
    CHECK(window_size(SDL_GetWindowFromID(overlay.id())) == std::array{1234, 234});
    CHECK((SDL_GetWindowFlags(SDL_GetWindowFromID(overlay.id())) & SDL_WINDOW_HIDDEN) != 0);
}

TEST_CASE("overlay minimum height follows content and scale without shrinking manual sizes") {
    const VideoSession video;
    const auto compiled = beacon::compile_template_json(
        R"({"name_key":"template.test","minecraft":{"min_version":0,"max_version":99999999},
            "goals":[{"id":"goal","fact":"a","view":{"name_key":"goal"}}],"completion_rule":"goal"})");
    REQUIRE(compiled);
    const auto layout = beacon::compile_layout_json(
        R"({"main":[{"nodes":["goal"],"height":20}],
            "overlay":[{"nodes":["goal"],"height":20,"margin":2,"padding":3}]})",
        *compiled);
    REQUIRE(layout);
    auto state = std::make_shared<beacon::PublishedState>();
    state->compiled = std::make_shared<const beacon::CompiledTemplate>(*compiled);
    state->localization = std::make_shared<const beacon::Localization>();
    state->layout = std::make_shared<const beacon::Layout>(*layout);
    state->snapshot.results.resize(compiled->graph.nodes.size());
    beacon::ProgressViewModel view;
    view.update(*state);
    beacon::WindowRenderer overlay("Overlay", {.width = 1234, .height = 20}, true, BEACON_TEST_ROOT, view, 1, false);
    REQUIRE(overlay.ready());
    beacon::Settings settings;
    const auto render = [&] {
        overlay.prepare(state, {});
        // Settle the body overhead after growing from a window shorter than its header and footer.
        for (int frame = 0; frame < 3; ++frame) {
            overlay.render(state, {}, &settings);
            REQUIRE(SDL_SyncWindow(SDL_GetWindowFromID(overlay.id())));
        }
    };
    const auto minimum_height = [&] {
        int width = 0;
        int height = 0;
        REQUIRE(SDL_GetWindowMinimumSize(SDL_GetWindowFromID(overlay.id()), &width, &height));
        CHECK(width == 0);
        return height;
    };
    render();
    const auto baseline = minimum_height();
    CHECK(baseline > 20);
    auto* window = SDL_GetWindowFromID(overlay.id());
    CHECK(window_size(window) == std::array{1234, baseline});
    REQUIRE(SDL_SetWindowSize(window, 1234, baseline + 100));
    render();
    CHECK(window_size(window) == std::array{1234, baseline + 100});
    settings.overlay_window_scale = 2;
    render();
    CHECK(minimum_height() > baseline);
    CHECK(window_size(window)[1] >= minimum_height());
    const auto taller = window_size(window);
    settings.overlay_window_scale = 0.5F;
    render();
    CHECK(minimum_height() < baseline);
    CHECK(window_size(window) == taller);
    auto expanded = std::make_shared<beacon::Layout>(*layout);
    expanded->overlay.front().height = 0.5F;
    state->layout = expanded;
    view.update(*state);
    render();
    CHECK(window_size(window) == taller);
    CHECK(minimum_height() < baseline);

    beacon::WindowRenderer main("Main", {.width = 80, .height = 20}, false, BEACON_TEST_ROOT, view);
    REQUIRE(main.ready());
    main.prepare(state, {});
    main.render(state, {}, &settings);
    CHECK(window_size(SDL_GetWindowFromID(main.id())) == std::array{80, 20});
    overlay.render({}, {}, &settings);
    CHECK(minimum_height() == 0);
    CHECK(window_size(window) == taller);
}
