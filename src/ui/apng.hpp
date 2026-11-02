#pragma once

#include <filesystem>
#include <string_view>

struct IMG_Animation;

namespace beacon {

// Whether `png` is an animated PNG: an acTL chunk comes before the image data.
bool is_animated_png(std::string_view png);

// Decodes an animated PNG into full-size RGBA frames, composed as the format describes, for IMG_FreeAnimation to free.
// SDL_image decodes animated PNGs only with libpng, so frames are rebuilt as plain PNGs for its built-in decoder.
// Returns nullptr and sets the SDL error when the file is not a valid animated PNG.
IMG_Animation* load_animated_png(std::string_view png);

// Whether an icon file plays as an animation: a GIF, or a PNG with animation frames.
bool is_animated_image(const std::filesystem::path& path);

// Decodes an animated icon file, for IMG_FreeAnimation to free.
IMG_Animation* load_animation(const std::filesystem::path& path);

}  // namespace beacon
