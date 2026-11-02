#include "../src/ui/apng.hpp"

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace {

void append_u32(std::string& out, const std::uint32_t value) {
    for (const int shift : {24, 16, 8, 0})
        out.push_back(static_cast<char>((value >> shift) & 0xFF));
}

void append_u16(std::string& out, const std::uint16_t value) {
    out.push_back(static_cast<char>(value >> 8));
    out.push_back(static_cast<char>(value & 0xFF));
}

void chunk(std::string& png, const std::string& type, const std::string& data) {
    append_u32(png, static_cast<std::uint32_t>(data.size()));
    png += type + data;
    append_u32(png, SDL_crc32(SDL_crc32(0, type.data(), type.size()), data.data(), data.size()));
}

// RGBA rows as zlib data in one stored (uncompressed) deflate block.
std::string image_data(const std::uint32_t width, const std::vector<std::uint32_t>& pixels) {
    std::string raw;
    for (std::size_t index = 0; index < pixels.size(); ++index) {
        if (index % width == 0)
            raw.push_back('\0');
        append_u32(raw, pixels[index]);
    }
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (const char c : raw) {
        a = (a + static_cast<unsigned char>(c)) % 65521;
        b = (b + a) % 65521;
    }
    std::string zlib{"\x78\x01\x01", 3};
    zlib.push_back(static_cast<char>(raw.size() & 0xFF));
    zlib.push_back(static_cast<char>(raw.size() >> 8));
    zlib.push_back(static_cast<char>(~raw.size() & 0xFF));
    zlib.push_back(static_cast<char>((~raw.size() >> 8) & 0xFF));
    zlib += raw;
    append_u32(zlib, b << 16 | a);
    return zlib;
}

std::string frame_control(const std::uint32_t sequence, const std::uint32_t width, const std::uint32_t height,
                          const std::uint32_t x, const std::uint32_t y, const std::uint16_t delay_num,
                          const std::uint16_t delay_den, const std::uint8_t dispose, const std::uint8_t blend) {
    std::string data;
    for (const auto value : {sequence, width, height, x, y})
        append_u32(data, value);
    append_u16(data, delay_num);
    append_u16(data, delay_den);
    data.push_back(static_cast<char>(dispose));
    data.push_back(static_cast<char>(blend));
    return data;
}

std::string header(const bool animated) {
    std::string png{"\x89PNG\r\n\x1a\n", 8};
    std::string ihdr;
    append_u32(ihdr, 2);
    append_u32(ihdr, 2);
    ihdr += std::string{"\x08\x06\x00\x00\x00", 5};
    chunk(png, "IHDR", ihdr);
    if (animated) {
        std::string actl;
        append_u32(actl, 3);
        append_u32(actl, 0);
        chunk(png, "acTL", actl);
    }
    return png;
}

constexpr std::uint32_t red = 0xFF0000FF;
constexpr std::uint32_t green = 0x00FF00FF;
constexpr std::uint32_t clear = 0;

// A 2x2 red frame, then a green pixel at (1, 1) that is cleared afterwards, then a green pixel at (0, 0) drawn over
// what is left.
std::string animated_png() {
    auto png = header(true);
    chunk(png, "fcTL", frame_control(0, 2, 2, 0, 0, 50, 1000, 0, 0));
    chunk(png, "IDAT", image_data(2, {red, red, red, red}));
    chunk(png, "fcTL", frame_control(1, 1, 1, 1, 1, 1, 10, 1, 0));
    std::string fdat;
    append_u32(fdat, 2);
    chunk(png, "fdAT", fdat + image_data(1, {green}));
    chunk(png, "fcTL", frame_control(3, 1, 1, 0, 0, 1, 0, 0, 1));
    fdat.clear();
    append_u32(fdat, 4);
    chunk(png, "fdAT", fdat + image_data(1, {green}));
    chunk(png, "IEND", "");
    return png;
}

std::uint32_t pixel(const SDL_Surface* surface, const int x, const int y) {
    const auto* row =
        static_cast<const std::uint8_t*>(surface->pixels) + static_cast<std::ptrdiff_t>(y) * surface->pitch;
    const auto* rgba = row + static_cast<std::ptrdiff_t>(x) * 4;
    return static_cast<std::uint32_t>(rgba[0]) << 24 | static_cast<std::uint32_t>(rgba[1]) << 16 |
           static_cast<std::uint32_t>(rgba[2]) << 8 | rgba[3];
}

}  // namespace

TEST_CASE("animated PNG frames are composed with their dispose and blend ops") {
    const auto png = animated_png();
    REQUIRE(beacon::is_animated_png(png));
    const std::unique_ptr<IMG_Animation, decltype(&IMG_FreeAnimation)> animation(beacon::load_animated_png(png),
                                                                                 IMG_FreeAnimation);
    REQUIRE(animation);
    REQUIRE(animation->count == 3);
    CHECK(animation->w == 2);
    CHECK(animation->h == 2);
    CHECK(animation->delays[0] == 50);
    CHECK(animation->delays[1] == 100);
    CHECK(animation->delays[2] == 10);

    CHECK(pixel(animation->frames[0], 1, 1) == red);
    CHECK(pixel(animation->frames[1], 1, 1) == green);
    CHECK(pixel(animation->frames[1], 0, 0) == red);
    // Frame 1 cleared its pixel to transparent before frame 2 was drawn.
    CHECK(pixel(animation->frames[2], 1, 1) == clear);
    CHECK(pixel(animation->frames[2], 0, 0) == green);
}

TEST_CASE("still and broken PNGs are not animations") {
    auto still = header(false);
    chunk(still, "IDAT", image_data(2, {red, red, red, red}));
    chunk(still, "IEND", "");
    CHECK_FALSE(beacon::is_animated_png(still));
    CHECK_FALSE(beacon::is_animated_png("not a png"));

    auto png = animated_png();
    CHECK(beacon::load_animated_png(png.substr(0, png.size() - 20)) == nullptr);
    // A frame that reaches past the image is rejected rather than drawn out of bounds.
    auto outside = header(true);
    chunk(outside, "fcTL", frame_control(0, 2, 2, 1, 0, 1, 10, 0, 0));
    chunk(outside, "IDAT", image_data(2, {red, red, red, red}));
    chunk(outside, "IEND", "");
    CHECK(beacon::load_animated_png(outside) == nullptr);
}
