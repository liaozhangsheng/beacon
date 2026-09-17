#include "../src/ui/window_context.hpp"

#include <beacon/ui/widgets.hpp>
#include <imgui_internal.h>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

TEST_CASE("window contexts scale from baseline and isolate mouse input") {
    REQUIRE(SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy"));
    REQUIRE(SDL_InitSubSystem(SDL_INIT_VIDEO));
    struct VideoSession {
        ~VideoSession() {
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
        }
    } session;
    beacon::WindowContext tracker("Tracker", {}, false);
    REQUIRE(tracker.ready());
    const auto baseline = ImGui::GetStyle();
    tracker.set_scale(2.0F);
    CHECK(ImGui::GetStyle().ScrollbarSize == Catch::Approx(baseline.ScrollbarSize * 2.0F));
    CHECK(ImGui::GetStyle().GrabMinSize == Catch::Approx(baseline.GrabMinSize * 2.0F));
    tracker.set_scale(1.0F);
    CHECK(ImGui::GetStyle().ScrollbarSize == baseline.ScrollbarSize);
    CHECK(ImGui::GetStyle().WindowPadding.x == baseline.WindowPadding.x);

    beacon::WindowContext overlay("Overlay", {}, true, 1.0F, false);
    REQUIRE(overlay.ready());
    CHECK((ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_NoMouseCursorChange) != 0);
    tracker.make_current();
    CHECK((ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_NoMouseCursorChange) == 0);

    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    event.button.windowID = tracker.id();
    event.button.button = SDL_BUTTON_LEFT;
    tracker.process_event(event);
    overlay.process_event(event);
    const auto frame = [](beacon::WindowContext& window, bool mouse_down) {
        window.make_current();
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        CHECK(ImGui::GetIO().MouseDown[0] == mouse_down);
        ImGui::EndFrame();
    };
    frame(tracker, true);
    frame(overlay, false);
    event.type = SDL_EVENT_MOUSE_BUTTON_UP;
    tracker.process_event(event);
    overlay.process_event(event);
    frame(tracker, false);
    frame(overlay, false);
}

TEST_CASE("window contexts can start hidden without becoming always-on-top") {
    REQUIRE(SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy"));
    REQUIRE(SDL_InitSubSystem(SDL_INIT_VIDEO));
    struct VideoSession {
        ~VideoSession() {
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
        }
    } session;

    beacon::WindowContext overlay("Overlay", {}, true, 1.0F, false, false);
    REQUIRE(overlay.ready());
    const auto flags = SDL_GetWindowFlags(overlay.window());
    CHECK((flags & SDL_WINDOW_HIDDEN) != 0);
    CHECK((flags & SDL_WINDOW_ALWAYS_ON_TOP) == 0);
}

TEST_CASE("text input moves its cursor through SDL keyboard events with an overlay") {
    REQUIRE(SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy"));
    REQUIRE(SDL_InitSubSystem(SDL_INIT_VIDEO));
    struct VideoSession {
        ~VideoSession() {
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
        }
    } session;
    beacon::WindowContext tracker("Tracker", {}, false);
    beacon::WindowContext overlay("Overlay", {}, true, 1.0F, false);
    REQUIRE(tracker.ready());
    REQUIRE(overlay.ready());
    tracker.make_current();
    ImGui::GetIO().ConfigMacOSXBehaviors = GENERATE(false, true);
    char text[64] = "abcdef";
    ImGuiID input_id = 0;
    const auto frame = [&](bool focus = false) {
        tracker.make_current();
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        ImGui::Begin("Settings");
        if (focus)
            ImGui::SetKeyboardFocusHere();
        input_id = ImGui::GetID("##path");
        beacon::textured_input(nullptr, nullptr, "##path", text, sizeof(text));
        ImGui::End();
        ImGui::Render();
        overlay.make_current();
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        ImGui::Render();
        tracker.make_current();
    };
    frame(true);
    frame();
    REQUIRE(ImGui::GetInputTextState(input_id));
    const auto key = [&](SDL_Keycode code, SDL_Scancode scan) {
        SDL_Event event{};
        event.type = SDL_EVENT_KEY_DOWN;
        event.key.windowID = tracker.id();
        event.key.key = code;
        event.key.scancode = scan;
        tracker.process_event(event);
        overlay.process_event(event);
        event.type = SDL_EVENT_KEY_UP;
        tracker.process_event(event);
        overlay.process_event(event);
        frame();
        frame();
    };
    key(SDLK_END, SDL_SCANCODE_END);
    REQUIRE(ImGui::GetInputTextState(input_id)->GetCursorPos() == 6);
    key(SDLK_LEFT, SDL_SCANCODE_LEFT);
    CHECK(ImGui::GetInputTextState(input_id)->GetCursorPos() == 5);
    key(SDLK_RIGHT, SDL_SCANCODE_RIGHT);
    CHECK(ImGui::GetInputTextState(input_id)->GetCursorPos() == 6);
}
