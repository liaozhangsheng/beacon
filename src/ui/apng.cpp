#include "apng.hpp"

#include <beacon/io/file.hpp>

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace beacon {
namespace {

constexpr std::string_view png_signature{"\x89PNG\r\n\x1a\n", 8};
// Bounds decoding work for a hostile or broken file.
constexpr std::uint32_t max_side = 4096;
constexpr std::size_t max_frames = 1024;
constexpr std::uint64_t max_frame_pixels = 16ULL * 1024 * 1024;
constexpr std::size_t max_file_bytes = 16 * 1024 * 1024;

struct Chunk {
    std::string_view type;
    std::string_view data;
};

std::uint32_t read_u32(const std::string_view bytes, const std::size_t at) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at])) << 24 |
           static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 1])) << 16 |
           static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 2])) << 8 |
           static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 3]));
}

std::uint16_t read_u16(const std::string_view bytes, const std::size_t at) {
    return static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[at]) << 8 |
                                      static_cast<unsigned char>(bytes[at + 1]));
}

void append_u32(std::string& out, const std::uint32_t value) {
    for (const int shift : {24, 16, 8, 0})
        out.push_back(static_cast<char>((value >> shift) & 0xFF));
}

void append_chunk(std::string& out, const std::string_view type, const std::string_view data) {
    append_u32(out, static_cast<std::uint32_t>(data.size()));
    out.append(type);
    out.append(data);
    append_u32(out, SDL_crc32(SDL_crc32(0, type.data(), type.size()), data.data(), data.size()));
}

// The chunks up to IEND, or nullopt when the file is not a well-formed PNG.
std::optional<std::vector<Chunk>> read_chunks(const std::string_view png) {
    if (!png.starts_with(png_signature))
        return std::nullopt;
    std::vector<Chunk> chunks;
    std::size_t at = png_signature.size();
    while (png.size() - at >= 12) {
        const auto size = read_u32(png, at);
        if (size > png.size() - at - 12)
            return std::nullopt;
        chunks.push_back({png.substr(at + 4, 4), png.substr(at + 8, size)});
        at += 12 + std::size_t{size};
        if (chunks.back().type == "IEND")
            return chunks;
    }
    return std::nullopt;
}

struct Frame {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    int delay_ms = 0;
    std::uint8_t dispose = 0;
    std::uint8_t blend = 0;
    std::vector<std::string_view> data;
};

constexpr std::uint8_t dispose_previous = 2;
constexpr std::uint8_t blend_source = 0;

struct SurfaceDeleter {
    void operator()(SDL_Surface* surface) const noexcept {
        SDL_DestroySurface(surface);
    }
};
using SurfacePtr = std::unique_ptr<SDL_Surface, SurfaceDeleter>;

struct AnimationDeleter {
    void operator()(IMG_Animation* animation) const noexcept {
        IMG_FreeAnimation(animation);
    }
};

// One frame rebuilt as a still PNG: the file's header and palette chunks with the frame's size and image data.
SurfacePtr decode_frame(const std::string_view ihdr, const std::vector<Chunk>& header_chunks, const Frame& frame) {
    std::string png(png_signature);
    std::string header;
    append_u32(header, frame.width);
    append_u32(header, frame.height);
    header.append(ihdr.substr(8));
    append_chunk(png, "IHDR", header);
    for (const auto& chunk : header_chunks)
        append_chunk(png, chunk.type, chunk.data);
    for (const auto data : frame.data)
        append_chunk(png, "IDAT", data);
    append_chunk(png, "IEND", {});
    auto* const io = SDL_IOFromConstMem(png.data(), png.size());
    SurfacePtr decoded(io == nullptr ? nullptr : IMG_LoadTyped_IO(io, true, "PNG"));
    if (!decoded)
        return nullptr;
    return SurfacePtr(SDL_ConvertSurface(decoded.get(), SDL_PIXELFORMAT_RGBA32));
}

IMG_Animation* fail(const char* message) {
    SDL_SetError("invalid animated PNG: %s", message);
    return nullptr;
}

}  // namespace

bool is_animated_png(const std::string_view png) {
    const auto chunks = read_chunks(png);
    if (!chunks)
        return false;
    for (const auto& chunk : *chunks) {
        if (chunk.type == "acTL")
            return true;
        if (chunk.type == "IDAT")
            return false;
    }
    return false;
}

IMG_Animation* load_animated_png(const std::string_view png) {
    const auto chunks = read_chunks(png);
    if (!chunks || chunks->empty() || chunks->front().type != "IHDR" || chunks->front().data.size() != 13)
        return fail("missing image header");
    const auto ihdr = chunks->front().data;
    const auto width = read_u32(ihdr, 0);
    const auto height = read_u32(ihdr, 4);
    if (width == 0 || height == 0 || width > max_side || height > max_side)
        return fail("unsupported image size");

    // Chunks that describe pixels (palette, transparency, colour space) precede the image data and apply to every
    // frame.
    std::vector<Chunk> header_chunks;
    std::vector<Frame> frames;
    bool seen_image_data = false;
    for (const auto& chunk : *chunks) {
        if (chunk.type == "fcTL") {
            if (chunk.data.size() != 26 || frames.size() == max_frames)
                return fail("bad frame control");
            const auto denominator = read_u16(chunk.data, 22);
            Frame frame{.width = read_u32(chunk.data, 4),
                        .height = read_u32(chunk.data, 8),
                        .x = read_u32(chunk.data, 12),
                        .y = read_u32(chunk.data, 16),
                        .delay_ms = read_u16(chunk.data, 20) * 1000 / (denominator == 0 ? 100 : denominator),
                        .dispose = static_cast<std::uint8_t>(chunk.data[24]),
                        .blend = static_cast<std::uint8_t>(chunk.data[25])};
            if (frame.width == 0 || frame.height == 0 || frame.x > width || frame.y > height ||
                frame.width > width - frame.x || frame.height > height - frame.y)
                return fail("frame outside the image");
            frames.push_back(std::move(frame));
        } else if (chunk.type == "IDAT") {
            seen_image_data = true;
            // Image data without a frame control before it is a still image that is not part of the animation.
            if (!frames.empty())
                frames.back().data.push_back(chunk.data);
        } else if (chunk.type == "fdAT") {
            if (frames.empty() || chunk.data.size() < 4)
                return fail("frame data without a frame");
            frames.back().data.push_back(chunk.data.substr(4));
        } else if (!seen_image_data && chunk.type != "IHDR" && chunk.type != "acTL") {
            header_chunks.push_back(chunk);
        }
    }
    if (frames.empty() || static_cast<std::uint64_t>(width) * height * frames.size() > max_frame_pixels)
        return fail("no frames or too many pixels");

    SurfacePtr canvas(SDL_CreateSurface(static_cast<int>(width), static_cast<int>(height), SDL_PIXELFORMAT_RGBA32));
    if (!canvas || !SDL_ClearSurface(canvas.get(), 0, 0, 0, 0))
        return nullptr;
    std::unique_ptr<IMG_Animation, AnimationDeleter> animation(
        static_cast<IMG_Animation*>(SDL_calloc(1, sizeof(IMG_Animation))));
    if (!animation)
        return nullptr;
    animation->w = static_cast<int>(width);
    animation->h = static_cast<int>(height);
    animation->frames = static_cast<SDL_Surface**>(SDL_calloc(frames.size(), sizeof(SDL_Surface*)));
    animation->delays = static_cast<int*>(SDL_calloc(frames.size(), sizeof(int)));
    if (animation->frames == nullptr || animation->delays == nullptr)
        return nullptr;

    for (std::size_t index = 0; index < frames.size(); ++index) {
        const auto& frame = frames[index];
        if (frame.data.empty())
            return fail("frame without image data");
        auto pixels = decode_frame(ihdr, header_chunks, frame);
        if (!pixels)
            return nullptr;
        // Each frame is drawn over what the previous one left behind, which its dispose op decides.
        SurfacePtr shown(SDL_DuplicateSurface(canvas.get()));
        SDL_Rect area{static_cast<int>(frame.x), static_cast<int>(frame.y), static_cast<int>(frame.width),
                      static_cast<int>(frame.height)};
        if (!shown ||
            !SDL_SetSurfaceBlendMode(pixels.get(),
                                     frame.blend == blend_source ? SDL_BLENDMODE_NONE : SDL_BLENDMODE_BLEND) ||
            !SDL_BlitSurface(pixels.get(), nullptr, shown.get(), &area))
            return nullptr;
        // The first frame has nothing earlier to return to, so it clears instead.
        if (frame.dispose != dispose_previous || index == 0) {
            canvas.reset(SDL_DuplicateSurface(shown.get()));
            if (!canvas || (frame.dispose != 0 && !SDL_FillSurfaceRect(canvas.get(), &area, 0)))
                return nullptr;
        }
        animation->frames[index] = shown.release();
        animation->delays[index] = frame.delay_ms;
        ++animation->count;
    }
    return animation.release();
}

bool is_animated_image(const std::filesystem::path& path) {
    const auto extension = path.extension();
    if (extension == ".gif" || extension == ".GIF")
        return true;
    if (extension != ".png" && extension != ".PNG")
        return false;
    const auto png = read_bounded_file(path, "icon", max_file_bytes);
    return png && is_animated_png(*png);
}

IMG_Animation* load_animation(const std::filesystem::path& path) {
    const auto file = path_to_utf8(path);
    const auto extension = path.extension();
    if (extension == ".gif" || extension == ".GIF")
        return IMG_LoadAnimation(file.c_str());
    const auto png = read_bounded_file(path, "icon", max_file_bytes);
    if (!png) {
        SDL_SetError("cannot read %s: %s", file.c_str(), png.error().message.c_str());
        return nullptr;
    }
    return load_animated_png(*png);
}

}  // namespace beacon
