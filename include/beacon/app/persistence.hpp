#pragma once

#include <beacon/core/model.hpp>

#include <array>
#include <filesystem>
#include <optional>
#include <string>

namespace beacon {

struct WindowSettings {
    int x = 100;
    int y = 100;
    int width = 960;
    int height = 640;
};

inline constexpr float min_window_scale = 0.5F;
inline constexpr float max_window_scale = 2.0F;
inline constexpr int max_window_dimension = 16384;
inline constexpr std::array<float, 3> default_overlay_window_background_color{0.0F, 170.0F / 255.0F, 0.0F};

// settings.json requires both paths; omitted fields keep these defaults.
struct Settings {
    std::string ui_language = "zh";

    // Follow the game directory of the Minecraft instance in the foreground.
    bool auto_detect = true;
    std::filesystem::path game_root;
    std::filesystem::path template_path;
    // Empty selects the template's default language.
    std::string language;

    int main_window_width = 960;
    int main_window_height = 640;
    float main_window_scale = 0.5F;
    std::array<float, 3> main_window_background_color{51.0F / 255.0F, 57.0F / 255.0F, 63.0F / 255.0F};

    bool overlay_visible = true;
    int overlay_window_width = 1280;
    int overlay_window_height = 180;
    float overlay_window_scale = 1.0F;
    bool overlay_transparent = false;
    std::array<float, 3> overlay_window_background_color = default_overlay_window_background_color;
    bool overlay_scroll_right = false;
    float overlay_scroll_speed = 60.0F;
};

class Persistence {
public:
    explicit Persistence(std::filesystem::path root);

    ylt::expected<std::optional<Settings>, Error> load_settings() const;
    ylt::expected<void, Error> save_settings(const Settings& settings) const;

private:
    std::filesystem::path root_;
};

}  // namespace beacon
