#include "enchantment_glint.hpp"

#include <beacon/io/file.hpp>

#include <SDL3_image/SDL_image.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <numbers>

namespace beacon {
namespace {

// The glint is sampled once per icon pixel, like Minecraft's GUI at scale 1 and AATool's captured glints, so
// each pixel takes one flat tint. Large images are sampled down to this size.
constexpr int max_overlay_size = 128;
// Minecraft's glint shader discards texels below 0.1 alpha.
constexpr std::uint8_t min_alpha = 26;

constexpr float texture_texels = 128.0F;
// An icon covers 16 texels of the glint texture: one texel per pixel of a 16 px item. AATool's captured glints
// pulse across the whole item at this span, as broad streaks pass over it.
constexpr float icon_span = 16.0F / texture_texels;
// TextureTransform.setupGlintTexturing at the default glint speed of 0.5: the texture scrolls one tile every
// 27.5 s horizontally and every 7.5 s vertically, rotated by 10 degrees.
constexpr double horizontal_period_ms = 110000.0 / 4.0;
constexpr double vertical_period_ms = 30000.0 / 4.0;
constexpr float angle = std::numbers::pi_v<float> / 18.0F;
// The texture's brightness lies between 0.19 and 1, with mean 0.562 and standard deviation 0.136.
constexpr float min_brightness = 0.19F;
constexpr float mean_brightness = 0.562F;
constexpr float brightness_deviation = 0.136F;
// The texture is one hue, (0.615, 0.291, 1) times its brightness, drawn at full glint strength as in AATool's
// captures (Minecraft defaults to 0.75). Minecraft blends with SRC_COLOR * src + dst, squaring each channel,
// which keeps the dark streaks faint and tints the bright ones toward blue.
constexpr std::array channel_weights{255.0F * 0.615F * 0.615F, 255.0F * 0.291F * 0.291F, 255.0F};

using Streaks = EnchantmentGlint::Streaks;
static_assert(std::has_single_bit(EnchantmentGlint::streak_period), "negative texels wrap with a power-of-two period");

// Soft wide bands with finer lines between them, one sample per texel. Each family gets mean 0 and half the
// texture's brightness variance, so their sum spreads like Minecraft's texture.
Streaks make_streaks(std::uint32_t seed) {
    // xorshift32 keeps the pattern identical on every platform.
    const auto next = [&seed] {
        seed ^= seed << 13U;
        seed ^= seed >> 17U;
        seed ^= seed << 5U;
        return static_cast<float>(seed & 0xFFFFFFU) / static_cast<float>(0x1000000U);
    };
    struct Band {
        int count;
        float min_width;
        float max_width;
        float min_brightness;
        float max_brightness;
    };
    // Widths are in texels.
    constexpr std::array bands{Band{4, 2.5F, 5.0F, 0.6F, 1.0F}, Band{5, 1.2F, 2.5F, 0.3F, 0.55F},
                               Band{7, 0.5F, 0.9F, 0.12F, 0.3F}};
    constexpr auto period = static_cast<float>(EnchantmentGlint::streak_period);
    Streaks streaks{};
    for (const auto& band : bands) {
        for (int index = 0; index < band.count; ++index) {
            const float center = next() * period;
            const float width = band.min_width + (next() * (band.max_width - band.min_width));
            const float brightness = band.min_brightness + (next() * (band.max_brightness - band.min_brightness));
            for (std::size_t texel = 0; texel < streaks.size(); ++texel) {
                const float offset = std::abs(static_cast<float>(texel) - center);
                const float distance = std::min(offset, period - offset) / width;
                streaks[texel] += brightness * std::exp(-0.5F * distance * distance);
            }
        }
    }
    float mean = 0.0F;
    for (const float value : streaks)
        mean += value / period;
    float variance = 0.0F;
    for (const float value : streaks)
        variance += (value - mean) * (value - mean) / period;
    const float scale = brightness_deviation / std::sqrt(2.0F * std::max(variance, 1e-6F));
    for (auto& value : streaks)
        value = (value - mean) * scale;
    return streaks;
}

// Linear filtering across texels, as Minecraft samples its blurred glint texture.
float sample(const Streaks& streaks, const float texels) {
    const float whole = std::floor(texels);
    const auto index = static_cast<std::size_t>(static_cast<std::int64_t>(whole)) % streaks.size();
    const float first = streaks[index];
    return first + ((streaks[(index + 1) % streaks.size()] - first) * (texels - whole));
}

}  // namespace

EnchantmentGlint::EnchantmentGlint(SDL_Renderer* renderer)
    : renderer_(renderer), vertical_streaks_(make_streaks(0x9E3779B9U)), diagonal_streaks_(make_streaks(0x85EBCA6BU)) {}

SDL_Texture* EnchantmentGlint::overlay(const std::filesystem::path& icon, const std::uint64_t now_ms) {
    auto [found, inserted] = overlays_.try_emplace(icon);
    auto& overlay = found->second;
    if (inserted)
        load(overlay, icon);
    if (overlay.texture == nullptr)
        return nullptr;
    update(overlay, now_ms - (now_ms % frame_ms));
    return overlay.texture.get();
}

void EnchantmentGlint::load(Overlay& overlay, const std::filesystem::path& icon) const {
    if (icon.empty())
        return;
    const auto file = path_to_utf8(icon);
    // GIF icons use their first frame as the glint shape.
    auto* const surface = IMG_Load(file.c_str());
    if (surface == nullptr)
        return;
    const int size = std::min(std::max(surface->w, surface->h), max_overlay_size);
    overlay.size = size;
    overlay.mask.resize(static_cast<std::size_t>(size) * static_cast<std::size_t>(size));
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            Uint8 alpha = 0;
            SDL_ReadSurfacePixel(surface, x * surface->w / size, y * surface->h / size, nullptr, nullptr, nullptr,
                                 &alpha);
            overlay.mask[(static_cast<std::size_t>(y) * size) + x] = alpha >= min_alpha ? 255 : 0;
        }
    }
    SDL_DestroySurface(surface);
    // Different icons shimmer out of step, as items at different atlas positions do.
    const auto hash = std::filesystem::hash_value(icon);
    overlay.offset_u = static_cast<float>(hash & 0xFFFFU) / 65536.0F;
    overlay.offset_v = static_cast<float>((hash >> 16U) & 0xFFFFU) / 65536.0F;
    overlay.texture.reset(
        SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, size, size));
    SDL_SetTextureBlendMode(overlay.texture.get(), SDL_BLENDMODE_ADD);
    SDL_SetTextureScaleMode(overlay.texture.get(), SDL_SCALEMODE_NEAREST);
}

void EnchantmentGlint::update(Overlay& overlay, const std::uint64_t now_ms) const {
    if (overlay.updated_ms == now_ms)
        return;
    void* pixels = nullptr;
    int pitch = 0;
    if (!SDL_LockTexture(overlay.texture.get(), nullptr, &pixels, &pitch))
        return;
    overlay.updated_ms = now_ms;
    const auto milliseconds = static_cast<double>(now_ms);
    const auto scroll_u = static_cast<float>(std::fmod(milliseconds / horizontal_period_ms, 1.0));
    const auto scroll_v = static_cast<float>(std::fmod(milliseconds / vertical_period_ms, 1.0));
    const float cosine = std::cos(angle);
    const float sine = std::sin(angle);
    const float step = icon_span / static_cast<float>(overlay.size);
    const auto* mask = overlay.mask.data();
    for (int y = 0; y < overlay.size; ++y) {
        auto* pixel = static_cast<std::uint8_t*>(pixels) + (static_cast<std::ptrdiff_t>(y) * pitch);
        const float atlas_v = overlay.offset_v + ((static_cast<float>(y) + 0.5F) * step);
        for (int x = 0; x < overlay.size; ++x, pixel += 4) {
            const float atlas_u = overlay.offset_u + ((static_cast<float>(x) + 0.5F) * step);
            const float u = ((cosine * atlas_u) - (sine * atlas_v) - scroll_u) * texture_texels;
            const float v = ((sine * atlas_u) + (cosine * atlas_v) + scroll_v) * texture_texels;
            const float brightness =
                std::clamp(mean_brightness + sample(vertical_streaks_, u) + sample(diagonal_streaks_, u + (2.0F * v)),
                           min_brightness, 1.0F);
            for (std::size_t channel = 0; channel < channel_weights.size(); ++channel)
                pixel[channel] = static_cast<std::uint8_t>((channel_weights[channel] * brightness * brightness) + 0.5F);
            // Transparent pixels add nothing under additive blending.
            pixel[3] = *mask++;
        }
    }
    SDL_UnlockTexture(overlay.texture.get());
}

}  // namespace beacon
