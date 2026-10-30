#include "temporary_directory.hpp"

#include <beacon/io/file.hpp>
#include <beacon/ui/widgets.hpp>
#include <SDL3_image/SDL_image.h>
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cstring>
#include <fstream>
#include <memory>
#include <algorithm>

TEST_CASE("missing icons draw a tinted checkerboard without an image resource") {
    const std::unique_ptr<ImGuiContext, decltype(&ImGui::DestroyContext)> context(ImGui::CreateContext(),
                                                                                  ImGui::DestroyContext);
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {800, 600};
    io.Fonts->AddFontDefault();
    REQUIRE(io.Fonts->Build());
    ImGui::NewFrame();
    auto* draw = ImGui::GetForegroundDrawList();
    for (const auto tint : {IM_COL32_WHITE, IM_COL32(100, 100, 100, 128)}) {
        const int start = draw->VtxBuffer.Size;
        beacon::draw_missing_icon(draw, {100.25F, 100.25F}, 20, tint);
        // Sample each quadrant, including the tint used for muted items.
        for (const auto offset : {ImVec2{5, 5}, ImVec2{15, 5}, ImVec2{5, 15}, ImVec2{15, 15}}) {
            ImU32 color = 0;
            for (int vertex = start; vertex < draw->VtxBuffer.Size; vertex += 4) {
                const auto& min = draw->VtxBuffer[vertex].pos;
                const auto& max = draw->VtxBuffer[vertex + 2].pos;
                if (100.25F + offset.x > min.x && 100.25F + offset.x < max.x && 100.25F + offset.y > min.y &&
                    100.25F + offset.y < max.y)
                    color = draw->VtxBuffer[vertex].col;
            }
            const auto expected = offset.x == offset.y ? IM_COL32(255, 0, 255, 255) : IM_COL32(0, 0, 0, 255);
            CHECK(color == (tint & expected));
        }
    }
    ImGui::EndFrame();
}

TEST_CASE("missing and corrupt static or animated icons fail without scheduling animation frames") {
    const beacon::test::TemporaryDirectory temporary;
    const std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> surface(
        SDL_CreateSurface(32, 32, SDL_PIXELFORMAT_ARGB8888), SDL_DestroySurface);
    REQUIRE(surface);
    const std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)> renderer(
        SDL_CreateSoftwareRenderer(surface.get()), SDL_DestroyRenderer);
    REQUIRE(renderer);
    beacon::UiAssets assets(renderer.get(), temporary.path);
    for (const auto name : {"missing.png", "corrupt.png", "missing.gif", "corrupt.gif"}) {
        const auto path = temporary.path / name;
        if (std::string_view(name).starts_with("corrupt"))
            std::ofstream(path) << "not an image";
        assets.begin_frame();
        REQUIRE(assets.texture(path) == nullptr);
        REQUIRE(assets.animated_frame(path, 0) == nullptr);
        REQUIRE(assets.next_frame_ms() == UINT64_MAX);
    }
}

TEST_CASE("item strikethrough crosses the icon and each visible label line") {
    const std::unique_ptr<ImGuiContext, decltype(&ImGui::DestroyContext)> context(ImGui::CreateContext(),
                                                                                  ImGui::DestroyContext);
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {800, 600};
    io.Fonts->AddFontDefault();
    REQUIRE(io.Fonts->Build());
    ImGui::NewFrame();
    auto* draw = ImGui::GetForegroundDrawList();
    draw->Flags &= ~ImDrawListFlags_AntiAliasedLines;
    const auto color = IM_COL32(100, 100, 100, 255);
    for (const std::string label : {"AB\nC", "AB\r\nC", "", "AB\n\n"}) {
        CAPTURE(label);
        const int start = draw->VtxBuffer.Size;
        beacon::draw_item_strikethrough(draw, {100, 100}, 20, {126, 100}, label, color, 1);
        const bool multiline = label.ends_with('C');
        REQUIRE(draw->VtxBuffer.Size - start == (multiline ? 8 : 4));
        const auto check_line = [&](const int offset, const float left, const float right, const float y) {
            float min_x = draw->VtxBuffer[start + offset].pos.x;
            float max_x = min_x;
            float sum_y = 0;
            for (int vertex = 0; vertex < 4; ++vertex) {
                const auto& value = draw->VtxBuffer[start + offset + vertex];
                min_x = std::min(min_x, value.pos.x);
                max_x = std::max(max_x, value.pos.x);
                sum_y += value.pos.y;
                CHECK(value.col == color);
            }
            CHECK(min_x == Catch::Approx(left + 0.5F));
            CHECK(max_x == Catch::Approx(right + 0.5F));
            CHECK(sum_y / 4 == Catch::Approx(y + 0.5F));
        };
        const float width = ImGui::CalcTextSize("AB").x;
        check_line(0, 100, label.empty() ? 120 : 126 + width, 110);
        if (multiline) {
            const float second_width = ImGui::CalcTextSize("C").x;
            const float left = 126 + (width - second_width) * 0.5F;
            check_line(4, left, left + second_width, 100 + ImGui::GetFontSize() * 1.5F);
        }
    }
    ImGui::EndFrame();
}

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
    // Each line draws its shadow first, then the text: [shadow 1, text 1, shadow 2, text 2].
    REQUIRE(draw->VtxBuffer.Size == 32);
    CHECK(draw->VtxBuffer[24].pos.x == Catch::Approx(draw->VtxBuffer[8].pos.x));
    CHECK(draw->VtxBuffer[24].pos.y - draw->VtxBuffer[8].pos.y == Catch::Approx(ImGui::GetFontSize()));
    const float shadow = beacon::text_shadow_offset(ImGui::GetFont(), ImGui::GetFontSize());
    CHECK(shadow > 0.0F);
    CHECK(draw->VtxBuffer[0].pos.x - draw->VtxBuffer[8].pos.x == Catch::Approx(shadow));
    CHECK(draw->VtxBuffer[0].pos.y - draw->VtxBuffer[8].pos.y == Catch::Approx(shadow));
    CHECK(draw->VtxBuffer[0].col == beacon::text_shadow_color(IM_COL32_WHITE));
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
    const auto path = root / "test/fixtures/resources/animation.gif";
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
            const std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> expected(
                SDL_ConvertSurface(reference->frames[frame], first_surface->format), SDL_DestroySurface);
            REQUIRE(expected);
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
    std::filesystem::copy_file(beacon::path_from_utf8(BEACON_TEST_ROOT) / "test/fixtures/resources/animation.gif",
                               path);
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
