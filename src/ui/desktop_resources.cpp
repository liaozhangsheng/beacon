#include "desktop_renderer_internal.hpp"

#include <beacon/io/file.hpp>
#include <beacon/ui/sizing.hpp>

#include <SDL3_image/SDL_image.h>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace beacon {

WindowRenderer::Impl::Impl(const std::string& title, const WindowSettings& settings, const bool overlay,
                           const std::filesystem::path& asset_root, const ProgressViewModel& view, const float scale,
                           const bool overlay_transparent, const bool initially_visible)
    : title_(title), asset_root_(asset_root), overlay_(overlay), overlay_transparent_(overlay_transparent),
      scale_(scale), context_(title, settings, overlay, scale, overlay_transparent, initially_visible), view_(view) {
    if (!context_.ready())
        return;
    const auto icon_path = asset_root_ / "assets/beacon.png";
    const auto icon_file = path_to_utf8(icon_path);
    if (auto* icon = IMG_Load(icon_file.c_str())) {
        if (!SDL_SetWindowIcon(context_.window(), icon)) {
            SDL_Log("Could not set window icon: %s", SDL_GetError());
        }
        SDL_DestroySurface(icon);
    } else {
        SDL_Log("Could not load window icon %s: %s", icon_file.c_str(), SDL_GetError());
    }
    assets_.emplace(context_.renderer(), asset_root_);
    context_.make_current();
    load_cjk_font();
    ready_ = true;
}

void WindowRenderer::Impl::prepare_profile(const std::shared_ptr<const PlayerCard>& player_card) {
    if (player_card_ != player_card) {
        player_card_ = player_card;
        remote_avatar_texture_.reset();
        avatar_texture_ = nullptr;
        if (player_card_ && !player_card_->avatar.empty()) {
            auto* io = SDL_IOFromConstMem(player_card_->avatar.data(), player_card_->avatar.size());
            auto* surface = io == nullptr ? nullptr : IMG_Load_IO(io, true);
            if (surface != nullptr) {
                remote_avatar_texture_.reset(SDL_CreateTextureFromSurface(context_.renderer(), surface));
                SDL_DestroySurface(surface);
            }
        }
    }
    if (avatar_texture_ == nullptr) {
        avatar_texture_ = remote_avatar_texture_ != nullptr
                              ? remote_avatar_texture_.get()
                              : assets_->texture(asset_root_ / "assets/avatars/steve.png");
    }
}

void WindowRenderer::Impl::prepare_template(const std::shared_ptr<const PublishedState>& state) {
    if (state && compiled_.get() != state->compiled.get()) {
        const bool had_template = compiled_ != nullptr;
        const bool changed_rules = !compiled_ || !compiled_->same_rules(*state->compiled);
        compiled_ = state->compiled;
        if (changed_rules) {
            completion_.reset(state->snapshot.results.size());
            glow_brightness_.clear();
            completion_started_.reset();
        }
        auto icons = icon_file_stamps(*compiled_);
        if (had_template && icons && *icons == icon_files_)
            return;
        icon_files_ = icons ? std::move(*icons) : IconFiles{};
        atlas_texture_.reset();
        atlas_regions_.clear();
        assets_->clear();
        if (remote_avatar_texture_ == nullptr) {
            avatar_texture_ = nullptr;
        }
        glow_texture_ = nullptr;
        glow_brightness_.clear();
        std::vector<std::filesystem::path> atlas_paths;
        for (const auto& presentation : state->compiled->presentation_by_node) {
            if (presentation) {
                if (!presentation->icon_path.empty()) {
                    atlas_paths.push_back(path_from_utf8(presentation->icon_path));
                }
                if (!presentation->frame_obtained_path.empty()) {
                    atlas_paths.push_back(path_from_utf8(presentation->frame_obtained_path));
                }
                if (!presentation->frame_unobtained_path.empty()) {
                    atlas_paths.push_back(path_from_utf8(presentation->frame_unobtained_path));
                }
                if ((glow_texture_ == nullptr) && !presentation->frame_obtained_path.empty()) {
                    glow_texture_ = assets_->texture(path_from_utf8(presentation->frame_obtained_path).parent_path() /
                                                     "frame_glow.png");
                    if (glow_texture_ != nullptr) {
                        SDL_SetTextureBlendMode(glow_texture_, SDL_BLENDMODE_ADD);
                    }
                }
            }
        }
        build_atlas(atlas_paths);
    }
}

void WindowRenderer::Impl::build_atlas(const std::vector<std::filesystem::path>& paths) {
    struct Source {
        std::filesystem::path key;
        SDL_Texture* texture = nullptr;
        SDL_FRect destination{};
    };
    constexpr float atlas_width = 1024.0F;
    constexpr float padding = 1.0F;
    std::vector<Source> sources;
    std::unordered_set<std::filesystem::path> seen;
    float x = padding;
    float y = padding;
    float row_height = 0.0F;
    for (const auto& path : paths) {
        // Icon paths are canonicalized while loading the template. Avoid normalizing and
        // allocating another path on every frame when looking them up in the atlas.
        const auto key = path;
        if (path.extension() == ".gif" || path.extension() == ".GIF") {
            continue;
        }
        if (!seen.emplace(key).second) {
            continue;
        }
        auto* const texture = assets_->texture(path);
        float width = 0.0F;
        float height = 0.0F;
        if (texture == nullptr || !SDL_GetTextureSize(texture, &width, &height) ||
            width + (padding * 2) > atlas_width) {
            continue;
        }
        if (x + width + padding > atlas_width) {
            x = padding;
            y += row_height + padding;
            row_height = 0.0F;
        }
        sources.push_back({key, texture, {x, y, width, height}});
        x += width + padding;
        row_height = std::max(row_height, height);
    }
    const auto atlas_height = static_cast<int>(std::ceil(y + row_height + padding));
    if (sources.empty() || atlas_height <= 0) {
        return;
    }
    atlas_texture_.reset(SDL_CreateTexture(context_.renderer(), SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET,
                                           static_cast<int>(atlas_width), atlas_height));
    if (atlas_texture_ == nullptr) {
        return;
    }
    auto* const previous_target = SDL_GetRenderTarget(context_.renderer());
    if (!SDL_SetRenderTarget(context_.renderer(), atlas_texture_.get())) {
        SDL_Log("Could not select icon atlas: %s", SDL_GetError());
        atlas_texture_.reset();
        return;
    }
    // SDL3 keeps scale/viewport/clip state per target. Only draw color is shared.
    SDL_FColor previous_color{};
    SDL_GetRenderDrawColorFloat(context_.renderer(), &previous_color.r, &previous_color.g, &previous_color.b,
                                &previous_color.a);
    bool complete = SDL_SetRenderDrawColor(context_.renderer(), 0, 0, 0, 0) && SDL_RenderClear(context_.renderer());
    for (const auto& source : sources) {
        if (!complete)
            break;
        SDL_BlendMode blend_mode = SDL_BLENDMODE_BLEND;
        complete = SDL_GetTextureBlendMode(source.texture, &blend_mode) &&
                   SDL_SetTextureBlendMode(source.texture, SDL_BLENDMODE_NONE) &&
                   SDL_RenderTexture(context_.renderer(), source.texture, nullptr, &source.destination);
        complete = SDL_SetTextureBlendMode(source.texture, blend_mode) && complete;
        atlas_regions_.emplace(
            source.key,
            AtlasRegion{{source.destination.x / atlas_width, source.destination.y / static_cast<float>(atlas_height)},
                        {(source.destination.x + source.destination.w) / atlas_width,
                         (source.destination.y + source.destination.h) / static_cast<float>(atlas_height)}});
    }
    complete = SDL_SetRenderTarget(context_.renderer(), previous_target) && complete;
    SDL_SetRenderDrawColorFloat(context_.renderer(), previous_color.r, previous_color.g, previous_color.b,
                                previous_color.a);
    complete = SDL_SetTextureBlendMode(atlas_texture_.get(), SDL_BLENDMODE_BLEND) && complete;
    if (!complete) {
        SDL_Log("Could not build icon atlas: %s", SDL_GetError());
        atlas_regions_.clear();
        atlas_texture_.reset();
        return;  // Keep the source cache so draw_node_icon can use individual textures.
    }

    // ponytail: The atlas is now the sole owner of these pixels; release source
    // textures immediately instead of retaining a duplicate cache.
    for (const auto& source : sources) {
        if (source.texture != glow_texture_ && source.texture != avatar_texture_) {
            assets_->discard(source.key);
        }
    }
}

void WindowRenderer::Impl::load_cjk_font() {
    const auto font_size = ui::font_size(scale_);
    // Each renderer needs its own atlas, but the immutable font file can be shared.
    const auto add_font = [&](const std::filesystem::path& path, ImFontConfig config, const ImWchar* ranges = nullptr) {
        using FontFile = std::pair<std::shared_ptr<void>, int>;
        static std::unordered_map<std::filesystem::path, FontFile> files;
        auto found = files.find(path);
        if (found == files.end()) {
            std::size_t size = 0;
            const auto file = path_to_utf8(path);
            std::shared_ptr<void> data(SDL_LoadFile(file.c_str(), &size), SDL_free);
            if (!data || size == 0 || size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
                return static_cast<ImFont*>(nullptr);
            }
            found = files.emplace(path, FontFile{std::move(data), static_cast<int>(size)}).first;
        }
        config.FontDataOwnedByAtlas = false;
        return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(found->second.first.get(), found->second.second, font_size,
                                                          &config, ranges);
    };
    ImFontConfig base_config;
    base_config.SizePixels = font_size;
    base_config.RasterizerMultiply = 2.0F;
    const auto bundled_font = asset_root_ / "assets/fonts/Minecraft.otf";
    if (add_font(bundled_font, base_config) == nullptr) {
        ImGui::GetIO().Fonts->AddFontDefault(&base_config);
    }
    ImFontConfig config;
    config.MergeMode = true;
    config.SizePixels = font_size;
    config.RasterizerMultiply = base_config.RasterizerMultiply;
    const auto unifont = asset_root_ / "assets/fonts/Unifont.ttf";
    if (std::filesystem::exists(unifont)) {
        add_font(unifont, config, ImGui::GetIO().Fonts->GetGlyphRangesChineseSimplifiedCommon());
    }
}

}  // namespace beacon
