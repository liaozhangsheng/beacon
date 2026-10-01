#include "../src/ui/window_context.hpp"
#include "../src/ui/settings_panel.hpp"

#include <beacon/ui/widgets.hpp>
#include <imgui_internal.h>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>
#include <imgui_freetype.h>
#include <cmath>

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

TEST_CASE("settings language changes preserve the window and keep actions inside the book") {
    REQUIRE(SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy"));
    REQUIRE(SDL_InitSubSystem(SDL_INIT_VIDEO));
    struct VideoSession {
        ~VideoSession() {
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
        }
    } session;
    beacon::WindowContext window("Settings", {}, false);
    REQUIRE(window.ready());
    auto* fonts = ImGui::GetIO().Fonts;
    fonts->SetFontLoader(ImGuiFreeType::GetFontLoader());
    ImFontConfig latin;
    latin.FontLoader = ImFontAtlasGetFontLoaderForStbTruetype();
    latin.RasterizerMultiply = 2.0F;
    REQUIRE(fonts->AddFontFromFileTTF(BEACON_TEST_ROOT "/assets/fonts/Minecraft.otf", 16.0F, &latin));
    ImFontConfig cjk;
    cjk.MergeMode = true;
    cjk.RasterizerMultiply = 2.0F;
    cjk.FontLoader = ImGuiFreeType::GetFontLoader();
    cjk.FontLoaderFlags =
        ImGuiFreeTypeLoaderFlags_Bold | ImGuiFreeTypeLoaderFlags_Monochrome | ImGuiFreeTypeLoaderFlags_MonoHinting;
    REQUIRE(fonts->AddFontFromFileTTF(BEACON_TEST_ROOT "/assets/fonts/Unifont.ttf", 16.0F, &cjk,
                                      fonts->GetGlyphRangesChineseSimplifiedCommon()));
    beacon::UiAssets assets(window.renderer(), BEACON_TEST_ROOT);
    beacon::SettingsPanel panel;
    panel.open();
    beacon::Settings settings;
    ImGuiWindow* book = nullptr;
    for (const auto* language : {"zh", "en", "zh"}) {
        settings.ui_language = language;
        // A second frame lets the scrollbar settle after the text changes height.
        for (int frame = 0; frame < 2; ++frame) {
            window.make_current();
            ImGui_ImplSDLRenderer3_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
            panel.render(ImGui::GetMainViewport(), settings, nullptr, nullptr, nullptr, assets);
            auto* current = ImGui::FindWindowByName("###beacon-settings");
            REQUIRE(current);
            if (book)
                CHECK(current == book);
            book = current;
            const auto actions_end = book->DC.CursorPosPrevLine;
            CHECK(actions_end.y >= book->Pos.y);
            CHECK(actions_end.y + ImGui::GetFrameHeight() + 8.0F <= book->Pos.y + book->Size.y);
            CHECK(actions_end.x >= book->Pos.x);
            CHECK(actions_end.x <= book->Pos.x + book->Size.x);
            REQUIRE(book->DC.ChildWindows.Size == 1);
            CHECK(book->DC.ChildWindows[0]->Pos.y + book->DC.ChildWindows[0]->Size.y <= actions_end.y);
            if (frame == 1) {
                INFO(language);
                INFO("book scroll " << book->ScrollMax.y << ", body scroll " << book->DC.ChildWindows[0]->ScrollMax.y);
                CHECK(book->ScrollMax.y == 0.0F);
                CHECK(book->DC.ChildWindows[0]->ScrollMax.y == 0.0F);
            }
            ImGui::Render();
        }
    }
}

TEST_CASE("application shortcuts use Ctrl on Windows and Linux and Cmd on macOS") {
    ImGui::CreateContext();
    struct Context {
        ~Context() {
            ImGui::DestroyContext();
        }
    } context;
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {800, 600};
    io.ConfigMacOSXBehaviors = GENERATE(false, true);
    const bool physical_cmd = GENERATE(false, true);
    io.Fonts->AddFontDefault();
    REQUIRE(io.Fonts->Build());
    io.AddKeyEvent(physical_cmd ? ImGuiMod_Super : ImGuiMod_Ctrl, true);
    io.AddKeyEvent(ImGuiKey_R, true);
    ImGui::NewFrame();
    const bool expected = physical_cmd == io.ConfigMacOSXBehaviors;
    CHECK(io.KeyCtrl == expected);
    CHECK(ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_R) == expected);
    ImGui::EndFrame();
}

TEST_CASE("Chinese emboldening keeps Minecraft Latin unchanged") {
    ImGui::CreateContext();
    struct Context {
        ~Context() {
            ImGui::DestroyContext();
        }
    } context;
    auto* atlas = ImGui::GetIO().Fonts;
    atlas->SetFontLoader(ImGuiFreeType::GetFontLoader());
    ImFontConfig latin;
    latin.FontLoader = ImFontAtlasGetFontLoaderForStbTruetype();
    latin.RasterizerMultiply = 2.0F;
    auto* original = atlas->AddFontFromFileTTF(BEACON_TEST_ROOT "/assets/fonts/Minecraft.otf", 16.0F, &latin);
    auto* mixed = atlas->AddFontFromFileTTF(BEACON_TEST_ROOT "/assets/fonts/Minecraft.otf", 16.0F, &latin);
    REQUIRE(original);
    REQUIRE(mixed);
    ImFontConfig chinese;
    chinese.FontLoaderFlags = ImGuiFreeTypeLoaderFlags_Monochrome | ImGuiFreeTypeLoaderFlags_MonoHinting;
    auto* regular = atlas->AddFontFromFileTTF(BEACON_TEST_ROOT "/assets/fonts/Unifont.ttf", 16.0F, &chinese);
    REQUIRE(regular);
    chinese.MergeMode = true;
    chinese.DstFont = mixed;
    chinese.FontLoaderFlags |= ImGuiFreeTypeLoaderFlags_Bold;
    REQUIRE(atlas->AddFontFromFileTTF(BEACON_TEST_ROOT "/assets/fonts/Unifont.ttf", 16.0F, &chinese));
    for (const float size : {16.0F, 32.0F}) {
        auto* original_glyphs = original->GetFontBaked(size, 1.0F);
        auto* mixed_glyphs = mixed->GetFontBaked(size, 1.0F);
        auto* regular_glyphs = regular->GetFontBaked(size, 1.0F);
        REQUIRE(original_glyphs->FindGlyphNoFallback('O'));
        REQUIRE(mixed_glyphs->FindGlyphNoFallback('O'));
        REQUIRE(mixed_glyphs->FindGlyphNoFallback(0x4E2D));
        REQUIRE(regular_glyphs->FindGlyphNoFallback(0x4E2D));
        const auto weight = [&](const ImFontGlyph& glyph) {
            const auto* texture = atlas->TexData;
            unsigned int alpha = 0;
            for (int y = static_cast<int>(std::lround(glyph.V0 * texture->Height));
                 y < static_cast<int>(std::lround(glyph.V1 * texture->Height)); ++y)
                for (int x = static_cast<int>(std::lround(glyph.U0 * texture->Width));
                     x < static_cast<int>(std::lround(glyph.U1 * texture->Width)); ++x)
                    alpha +=
                        texture->Pixels[(x + y * texture->Width) * texture->BytesPerPixel + texture->BytesPerPixel - 1];
            return alpha;
        };
        const auto* original_o = original_glyphs->FindGlyphNoFallback('O');
        const auto* mixed_o = mixed_glyphs->FindGlyphNoFallback('O');
        CHECK(mixed_o->AdvanceX == original_o->AdvanceX);
        CHECK(weight(*mixed_o) == weight(*original_o));
        CHECK(weight(*mixed_glyphs->FindGlyphNoFallback(0x4E2D)) >
              weight(*regular_glyphs->FindGlyphNoFallback(0x4E2D)));
    }
}
