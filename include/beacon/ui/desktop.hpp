#pragma once

#include <beacon/app/persistence.hpp>
#include <beacon/app/runtime.hpp>

#include <SDL3/SDL_events.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <cstdint>
#include <vector>

namespace beacon {

class DesktopLoop {
public:
    DesktopLoop(Runtime* runtime, Persistence* persistence, std::shared_ptr<const PublishedState> fixed_state,
                const Settings& settings, const std::filesystem::path& asset_root,
                const std::filesystem::path& data_root, const std::vector<std::filesystem::path>& templates = {},
                int frame_limit = 0, std::optional<Error> initial_error = std::nullopt);
    ~DesktopLoop();
    DesktopLoop(const DesktopLoop&) = delete;
    DesktopLoop& operator=(const DesktopLoop&) = delete;

    [[nodiscard]] bool ready() const;
    [[nodiscard]] bool running() const;
    [[nodiscard]] std::uint32_t next_wake_timeout_ms() const;
    void process_event(const SDL_Event& event);
    bool iterate();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

int run_desktop(Runtime& runtime, Persistence& persistence, const Settings& settings,
                const std::filesystem::path& asset_root, const std::filesystem::path& data_root,
                const std::vector<std::filesystem::path>& templates = {}, int frame_limit = 0,
                std::optional<Error> initial_error = std::nullopt);
int desktop_smoke_test(const std::filesystem::path& asset_root);

}  // namespace beacon
