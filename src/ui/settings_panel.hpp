#pragma once

#include <beacon/app/persistence.hpp>
#include <beacon/ui/widgets.hpp>

#include <imgui.h>

#include <array>
#include <filesystem>
#include <optional>
#include <vector>

namespace beacon {

class SettingsPanel {
public:
    void open();
    void render(const ImGuiViewport* viewport, Settings& settings, std::optional<Error>* error,
                const std::vector<std::filesystem::path>* templates, bool* apply, UiAssets& assets);

private:
    void close();
    void refresh_languages(const std::filesystem::path& template_path);
    bool render_source(Settings& settings, const std::vector<std::filesystem::path>* templates, ImVec2 content_size,
                       float label_width, bool* apply, UiAssets& assets);
    static bool render_appearance(Settings& settings, ImVec2 content_size, float label_width, UiAssets& assets);
    void render_actions(ImVec2 padding, bool* apply, UiAssets& assets);

    bool open_ = false;
    std::array<char, max_string_bytes + 1> game_root_input_{};
    bool game_root_input_loaded_ = false;
    std::filesystem::path languages_template_path_;
    std::vector<std::string> languages_;
};

}  // namespace beacon
