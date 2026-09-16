#include "temporary_directory.hpp"

#include <beacon/io/file.hpp>
#include <beacon/ui/widgets.hpp>
#include <SDL3_image/SDL_image.h>
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cstring>
#include <fstream>
#include <memory>

TEST_CASE("scrolling text preserves the same fractional translation as images") {
    const std::unique_ptr<ImGuiContext, decltype(&ImGui::DestroyContext)> context(ImGui::CreateContext(),
                                                                                  ImGui::DestroyContext);
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {800, 600};
    io.Fonts->AddFontDefault();
    REQUIRE(io.Fonts->Build());
    ImGui::NewFrame();
    auto* draw = ImGui::GetForegroundDrawList();
    const ImVec2 origin{100.25F, 100.25F};
    beacon::draw_subpixel_text(draw, origin, IM_COL32_WHITE, "Title");
    const auto first = draw->VtxBuffer;
    REQUIRE(first.Size > 0);
    for (const float movement : {0.25F, 0.75F, -0.5F, -1.5F}) {
        const int start = draw->VtxBuffer.Size;
        beacon::draw_subpixel_text(draw, {origin.x + movement, origin.y + movement}, IM_COL32_WHITE, "Title");
        REQUIRE(draw->VtxBuffer.Size - start == first.Size);
        for (int index = 0; index < first.Size; ++index) {
            CHECK(draw->VtxBuffer[start + index].pos.x - first[index].pos.x == Catch::Approx(movement));
            CHECK(draw->VtxBuffer[start + index].pos.y - first[index].pos.y == Catch::Approx(movement));
            CHECK(draw->VtxBuffer[index].pos.x == first[index].pos.x);
        }
    }
    ImGui::EndFrame();
}

TEST_CASE("progress text draws centered lines and returns the height used by the next row") {
    const std::unique_ptr<ImGuiContext, decltype(&ImGui::DestroyContext)> context(ImGui::CreateContext(),
                                                                                  ImGui::DestroyContext);
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {800, 600};
    io.Fonts->AddFontDefault();
    REQUIRE(io.Fonts->Build());
    ImGui::NewFrame();
    auto* draw = ImGui::GetForegroundDrawList();
    constexpr float center = 100.25F;
    constexpr float top = 50.25F;
    REQUIRE(beacon::draw_progress_text(draw, "AA\nAA", center, top, IM_COL32_WHITE, 100) == 2);
    REQUIRE(draw->VtxBuffer.Size == 16);
    CHECK(draw->VtxBuffer[8].pos.x == Catch::Approx(draw->VtxBuffer[0].pos.x));
    CHECK(draw->VtxBuffer[8].pos.y - draw->VtxBuffer[0].pos.y == Catch::Approx(ImGui::GetFontSize()));
    for (const auto& [text, count] : {std::pair{"", 1U}, {"\n", 2U}, {"AA\n", 2U}, {"AA\r\nAA", 2U}, {"\r", 1U}}) {
        CAPTURE(text);
        CHECK(beacon::draw_progress_text(draw, text, center, top, IM_COL32_WHITE, 100) == count);
    }
    CHECK(beacon::draw_progress_text(draw, "ABC", center, top, IM_COL32_WHITE, 1) == 3);
    CHECK(beacon::draw_progress_text(draw, "中文", center, top, IM_COL32_WHITE, 1) == 2);
    ImGui::EndFrame();
}

TEST_CASE("GIF playback reuses one texture per renderer and preserves frames and timing") {
    const auto root = beacon::path_from_utf8(BEACON_TEST_ROOT);
    const auto path = root / "assets/vender/minecraft/item/enchanted_book.gif";
    const auto file = beacon::path_to_utf8(path);
    const std::unique_ptr<IMG_Animation, decltype(&IMG_FreeAnimation)> reference(IMG_LoadAnimation(file.c_str()),
                                                                                 IMG_FreeAnimation);
    REQUIRE(reference);
    REQUIRE(reference->count > 1);
    const auto surface = [&] {
        return std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)>(
            SDL_CreateSurface(reference->w, reference->h, SDL_PIXELFORMAT_ARGB8888), SDL_DestroySurface);
    };
    auto first_surface = surface();
    auto second_surface = surface();
    REQUIRE(first_surface);
    REQUIRE(second_surface);
    const std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)> first_renderer(
        SDL_CreateSoftwareRenderer(first_surface.get()), SDL_DestroyRenderer);
    const std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)> second_renderer(
        SDL_CreateSoftwareRenderer(second_surface.get()), SDL_DestroyRenderer);
    REQUIRE(first_renderer);
    REQUIRE(second_renderer);
    beacon::UiAssets first(first_renderer.get(), root);
    beacon::UiAssets second(second_renderer.get(), root);
    first.begin_frame();
    REQUIRE(first.next_frame_ms() == UINT64_MAX);
    auto* texture = first.animated_frame(path, 0);
    REQUIRE(texture);
    SDL_ScaleMode scale_mode;
    REQUIRE(SDL_GetTextureScaleMode(texture, &scale_mode));
    REQUIRE(scale_mode == SDL_SCALEMODE_LINEAR);
    auto* other_texture = second.animated_frame(path, 0);
    REQUIRE(other_texture);
    REQUIRE(texture != other_texture);

    std::uint64_t duration = 0;
    for (int frame = 0; frame < reference->count; ++frame) {
        duration += reference->delays[frame] > 0 ? reference->delays[frame] : 100;
    }
    std::uint64_t start = 0;
    for (int frame = 0; frame < reference->count; ++frame) {
        if (frame < 2 || frame == reference->count - 1) {
            CAPTURE(frame);
            first.begin_frame();
            REQUIRE(first.animated_frame(path, start) == texture);
            const auto delay =
                static_cast<std::uint64_t>(reference->delays[frame] > 0 ? reference->delays[frame] : 100);
            REQUIRE(first.next_frame_ms() == start + delay);
            first.begin_frame();
            REQUIRE(first.animated_frame(path, start + delay - 1) == texture);
            REQUIRE(first.next_frame_ms() == start + delay);
            REQUIRE(SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_NONE));
            REQUIRE(SDL_RenderTexture(first_renderer.get(), texture, nullptr, nullptr));
            REQUIRE(SDL_FlushRenderer(first_renderer.get()));
            const auto* expected = reference->frames[frame];
            for (int row = 0; row < expected->h; ++row) {
                REQUIRE(std::memcmp(static_cast<char*>(first_surface->pixels) + row * first_surface->pitch,
                                    static_cast<char*>(expected->pixels) + row * expected->pitch,
                                    static_cast<std::size_t>(expected->w) * 4) == 0);
            }
            first.begin_frame();
            REQUIRE(first.animated_frame(path, duration + start) == texture);
            REQUIRE(first.next_frame_ms() == duration + start + delay);
        }
        start += reference->delays[frame] > 0 ? reference->delays[frame] : 100;
    }
    first.clear();
    REQUIRE(second.animated_frame(path, duration) == other_texture);
}

TEST_CASE("reloading a replaced GIF invalidates shared decoding while another window still uses the old image") {
    const beacon::test::TemporaryDirectory temporary;
    const auto path = temporary.path / "icon.gif";
    std::filesystem::copy_file(
        beacon::path_from_utf8(BEACON_TEST_ROOT) / "assets/vender/minecraft/item/enchanted_book.gif", path);
    std::ifstream input(path, std::ios::binary);
    std::string data{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    input.close();
    const auto control = data.find(std::string("\x21\xf9\x04", 3));
    REQUIRE(control != std::string::npos);
    REQUIRE(control + 5 < data.size());
    const auto original_delay =
        (static_cast<unsigned char>(data[control + 4]) + 256U * static_cast<unsigned char>(data[control + 5])) * 10U;
    REQUIRE(original_delay != 1230);
    const auto stamp = std::filesystem::last_write_time(path);
    const std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> surface(
        SDL_CreateSurface(32, 32, SDL_PIXELFORMAT_ARGB8888), SDL_DestroySurface);
    REQUIRE(surface);
    const std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)> renderer(
        SDL_CreateSoftwareRenderer(surface.get()), SDL_DestroyRenderer);
    REQUIRE(renderer);
    beacon::UiAssets old_window(renderer.get(), temporary.path);
    REQUIRE(old_window.animated_frame(path, 0));
    data[control + 4] = 123;
    data[control + 5] = 0;
    const auto replacement = temporary.path / "replacement.gif";
    std::ofstream(replacement, std::ios::binary | std::ios::trunc).write(data.data(), data.size());
    std::filesystem::last_write_time(replacement, stamp);
    std::filesystem::rename(path, temporary.path / "old.gif");
    std::filesystem::rename(replacement, path);
    beacon::UiAssets reloaded_window(renderer.get(), temporary.path);
    REQUIRE(reloaded_window.animated_frame(path, 0));
    REQUIRE(reloaded_window.next_frame_ms() == 1230);
    REQUIRE(old_window.next_frame_ms() == (original_delay > 0 ? original_delay : 100));
}
