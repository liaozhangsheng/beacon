#pragma once

#include <beacon/app/persistence.hpp>
#include <beacon/app/runtime.hpp>

#include <SDL3/SDL.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace beacon {

struct PlayerCard;
struct ProgressViewModel;

class WindowRenderer {
public:
    WindowRenderer(const std::string& title, const WindowSettings& settings, bool overlay,
                   const std::filesystem::path& asset_root, const ProgressViewModel& view, float scale = 1.0F,
                   bool overlay_transparent = true, bool initially_visible = true);
    ~WindowRenderer();

    WindowRenderer(const WindowRenderer&) = delete;
    WindowRenderer& operator=(const WindowRenderer&) = delete;

    [[nodiscard]] bool ready() const;
    [[nodiscard]] SDL_WindowID id() const;
    [[nodiscard]] SDL_Renderer* renderer() const;

    [[nodiscard]] std::uint64_t refresh_delay_ms() const;

    [[nodiscard]] bool overlay_transparent() const;
    bool set_overlay_transparent(bool enabled);

    void process_event(const SDL_Event& event);
    void prepare(const std::shared_ptr<const PublishedState>& state,
                 const std::shared_ptr<const PlayerCard>& player_card);
    void render(const std::shared_ptr<const PublishedState>& state, const std::optional<Error>& runtime_error,
                Settings* settings = nullptr, std::optional<Error>* settings_error = nullptr,
                const std::vector<std::filesystem::path>* template_options = nullptr, bool* apply_settings = nullptr,
                Runtime* runtime = nullptr);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace beacon
