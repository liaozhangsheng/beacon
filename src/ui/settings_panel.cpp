#include "settings_panel.hpp"

#include <beacon/io/file.hpp>
#include <beacon/ui/display.hpp>
#include <beacon/ui/sizing.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace beacon {
namespace {

std::string template_name(const std::filesystem::path& path) {
    return path_to_utf8(path.parent_path().filename());
}

// minecraft_book.png is 146x180 texels; the window keeps that shape so texels stay square.
constexpr ImVec2 book_texels{146.0F, 180.0F};
constexpr float minimum_book_scale = 3.5F;
// Labels printed on the book's page.
constexpr ImVec4 book_text_color{0.0F, 0.0F, 0.0F, 1.0F};

void keep_book_proportions(ImGuiSizeCallbackData* data) {
    const float current = data->CurrentSize.x / book_texels.x;
    const float horizontal = data->DesiredSize.x / book_texels.x;
    const float vertical = data->DesiredSize.y / book_texels.y;
    // Follow whichever edge the user is dragging.
    float scale = std::abs(horizontal - current) >= std::abs(vertical - current) ? horizontal : vertical;
    const auto work = ImGui::GetMainViewport()->WorkSize;
    const float maximum =
        std::max(minimum_book_scale, std::min((work.x - 32.0F) / book_texels.x, (work.y - 32.0F) / book_texels.y));
    // Whole framebuffer pixels per texel keep the nearest-sampled art even.
    const float step = 1.0F / std::max(1.0F, ImGui::GetIO().DisplayFramebufferScale.y);
    scale = std::clamp(std::floor(scale / step) * step, minimum_book_scale, std::max(minimum_book_scale, maximum));
    data->DesiredSize = {book_texels.x * scale, book_texels.y * scale};
}

// Combo boxes are black vanilla fields: white text, and popups padded like the field itself.
void push_combo_style() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        {ImGui::GetStyle().WindowPadding.x, ImGui::GetStyle().FramePadding.y});
    ImGui::PushStyleColor(ImGuiCol_Text, {1.0F, 1.0F, 1.0F, 1.0F});
}

void pop_combo_style() {
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

void form_row(const char* label) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-1.0F);
}

}  // namespace

void SettingsPanel::open() {
    open_ = true;
    drag_grab_.reset();
    game_root_input_loaded_ = false;
    languages_template_path_.clear();
    languages_.clear();
}

void SettingsPanel::close() {
    open_ = false;
    drag_grab_.reset();
    game_root_input_loaded_ = false;
}

void SettingsPanel::refresh_languages(const std::filesystem::path& template_path) {
    if (languages_template_path_ == template_path) {
        return;
    }
    languages_template_path_ = template_path;
    languages_.clear();
    if (!template_path.empty()) {
        languages_ = available_template_languages(template_path);
    }
}

void SettingsPanel::render(const ImGuiViewport* viewport, Settings& settings, std::optional<Error>* error,
                           const std::vector<std::filesystem::path>* templates, bool* apply, UiAssets& assets) {
    if (!open_) {
        return;
    }
    ImGui::PushFont(nullptr, ui::settings_font_size);
    constexpr ImVec2 settings_size{book_texels.x * minimum_book_scale, book_texels.y * minimum_book_scale};
    const ImVec2 initial_padding{settings_size.x * (16.0F / 146.0F), settings_size.y * (12.0F / 180.0F)};
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, {0.5F, 0.5F});
    ImGui::SetNextWindowSize(settings_size, ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(settings_size,
                                        {std::max(settings_size.x, viewport->WorkSize.x - 32.0F),
                                         std::max(settings_size.y, viewport->WorkSize.y - 32.0F)},
                                        keep_book_proportions);
    // Move before Begin so the book, its text and its widgets all draw at the new position this frame;
    // moving mid-window leaves whatever was already drawn one frame behind.
    if (drag_grab_ && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const auto mouse = ImGui::GetIO().MousePos;
        ImGui::SetNextWindowPos({mouse.x - drag_grab_->x, mouse.y - drag_grab_->y});
    }
    // Combos and color fields mimic the vanilla text field: black body, #A0A0A0 frame.
    constexpr ImVec4 field_border{160.0F / 255.0F, 160.0F / 255.0F, 160.0F / 255.0F, 1.0F};
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, initial_padding);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 0.0F);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, {0.0F, 0.0F, 0.0F, 0.0F});
    ImGui::PushStyleColor(ImGuiCol_Text, book_text_color);
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, {0.25F, 0.25F, 0.25F, 1.0F});
    ImGui::PushStyleColor(ImGuiCol_Border, field_border);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, {0.0F, 0.0F, 0.0F, 1.0F});
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, {0.12F, 0.12F, 0.12F, 1.0F});
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, {0.0F, 0.0F, 0.0F, 1.0F});
    ImGui::PushStyleColor(ImGuiCol_PopupBg, {0.0F, 0.0F, 0.0F, 1.0F});
    ImGui::PushStyleColor(ImGuiCol_Header, {0.30F, 0.30F, 0.30F, 1.0F});
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, {0.40F, 0.40F, 0.40F, 1.0F});
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, {0.25F, 0.25F, 0.25F, 1.0F});
    ImGui::PushStyleColor(ImGuiCol_Button, {0.0F, 0.0F, 0.0F, 1.0F});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.12F, 0.12F, 0.12F, 1.0F});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, {0.0F, 0.0F, 0.0F, 1.0F});
    ImGui::PushStyleColor(ImGuiCol_ResizeGrip, {0.0F, 0.0F, 0.0F, 0.0F});
    ImGui::PushStyleColor(ImGuiCol_ResizeGripHovered, {0.0F, 0.0F, 0.0F, 0.0F});
    ImGui::PushStyleColor(ImGuiCol_ResizeGripActive, {0.0F, 0.0F, 0.0F, 0.0F});
    ImGui::PushStyleColor(ImGuiCol_SeparatorHovered, {0.0F, 0.0F, 0.0F, 0.0F});
    ImGui::PushStyleColor(ImGuiCol_SeparatorActive, {0.0F, 0.0F, 0.0F, 0.0F});
    const auto restore_style = [] {
        ImGui::PopStyleColor(19);
        ImGui::PopStyleVar(6);
        ImGui::PopFont();
    };
    const bool window_visible = ImGui::Begin("设置##beacon", &open_,
                                             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                                 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings);
    if (!window_visible) {
        drag_grab_.reset();
        ImGui::End();
        restore_style();
        return;
    }
    const auto window_min = ImGui::GetWindowPos();
    const auto window_size = ImGui::GetWindowSize();
    const ImVec2 padding{window_size.x * (16.0F / 146.0F), window_size.y * (12.0F / 180.0F)};
    const ImVec2 content_size{window_size.x - (padding.x * 2.0F), 0.0F};
    const ImVec2 book_min{snap_to_pixel(window_min.x), snap_to_pixel(window_min.y)};
    ImGui::GetWindowDrawList()->AddImage(assets.ui("minecraft_book.png"), book_min,
                                         {book_min.x + window_size.x, book_min.y + window_size.y});
    ImGui::SetCursorPos({0.0F, 0.0F});
    ImGui::InvisibleButton("##settings-drag", {window_size.x, 44.0F});
    if (ImGui::IsItemActivated()) {
        // Track the grab point rather than accumulating deltas, which truncated window positions would drop.
        const auto mouse = ImGui::GetIO().MousePos;
        drag_grab_ = ImVec2{mouse.x - window_min.x, mouse.y - window_min.y};
    } else if (!ImGui::IsItemActive()) {
        drag_grab_.reset();
    }
    ImGui::SetCursorPos(padding);
    ImGui::TextUnformatted("Beacon 设置");
    ImGui::SetCursorPos({padding.x, padding.y + 44.0F});
    const char* const setting_labels[] = {
        "模板：",
        "语言：",
        "自动识别游戏目录：",
        "游戏目录：",
        "主窗口 缩放（倍率）：",
        "主窗口 背景色：",
        "Overlay 显示：",
        "Overlay 透明背景：",
        "Overlay 滚动方向：",
        "Overlay 缩放（倍率）：",
        "Overlay 背景色：",
        "Overlay 滚动速度：",
    };
    float label_width = 0.0F;
    for (const auto* label : setting_labels)
        label_width = std::max(label_width, ImGui::CalcTextSize(label).x);
    ImGui::TextUnformatted("数据来源");
    ImGui::SetCursorPosX(padding.x);
    bool changed = render_source(settings, templates, content_size, label_width, apply, assets);
    ImGui::SetCursorPosX(padding.x);
    ImGui::Separator();
    ImGui::SetCursorPosX(padding.x);
    ImGui::TextUnformatted("外观");
    ImGui::SetCursorPosX(padding.x);
    changed = render_appearance(settings, content_size, label_width, assets) || changed;
    if (changed && error != nullptr) {
        error->reset();
    }
    if (error != nullptr && *error) {
        ImGui::SetCursorPosX(padding.x);
        ImGui::Text("无法保存：%s", (*error)->message.c_str());
    }
    ImGui::SetCursorPosX(padding.x);
    ImGui::Separator();
    ImGui::SetCursorPosX(padding.x);
    ImGui::TextUnformatted("操作提示");
    ImGui::SetCursorPosX(padding.x);
    render_tips(content_size);
    render_actions(padding, apply, assets);
    ImGui::End();
    restore_style();
}

bool SettingsPanel::render_source(Settings& settings, const std::vector<std::filesystem::path>* templates,
                                  const ImVec2 content_size, const float label_width, bool* apply, UiAssets& assets) {
    if (!game_root_input_loaded_ || settings.auto_detect) {
        const auto value = path_to_utf8(settings.game_root);
        const auto length = std::min(value.size(), game_root_input_.size() - 1);
        std::copy_n(value.data(), length, game_root_input_.data());
        game_root_input_[length] = '\0';
        game_root_input_loaded_ = true;
    }
    if (!ImGui::BeginTable("##source-settings", 2, ImGuiTableFlags_SizingStretchProp, content_size)) {
        return false;
    }
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, label_width);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch);
    bool changed = false;
    // Template, language and auto-detect changes apply at once instead of waiting for Save.
    const auto apply_now = [&] {
        changed = true;
        if (apply != nullptr)
            *apply = true;
    };
    refresh_languages(settings.template_path);
    form_row("模板：");
    if (templates != nullptr && !templates->empty()) {
        int selected = 0;
        for (int index = 0; index < static_cast<int>(templates->size()); ++index) {
            if ((*templates)[index] == settings.template_path) {
                selected = index;
                break;
            }
        }
        const auto selected_name = template_name((*templates)[selected]);
        push_combo_style();
        if (ImGui::BeginCombo("##template", selected_name.c_str())) {
            for (int index = 0; index < static_cast<int>(templates->size()); ++index) {
                const auto name = template_name((*templates)[index]);
                if (ImGui::Selectable(name.c_str(), index == selected)) {
                    if (settings.template_path != (*templates)[index]) {
                        settings.template_path = (*templates)[index];
                        settings.language.clear();
                        refresh_languages(settings.template_path);
                        apply_now();
                    }
                }
            }
            ImGui::EndCombo();
        }
        pop_combo_style();
    } else {
        ImGui::TextUnformatted("没有可用模板");
    }
    form_row("语言：");
    int selected_language = 0;
    for (int index = 0; index < static_cast<int>(languages_.size()); ++index) {
        if (languages_[index] == settings.language) {
            selected_language = index + 1;
            break;
        }
    }
    push_combo_style();
    const auto selected_language_name =
        selected_language == 0 ? std::string_view{"默认"} : std::string_view{languages_[selected_language - 1]};
    if (ImGui::BeginCombo("##language", selected_language_name.data())) {
        if (ImGui::Selectable("默认", selected_language == 0)) {
            settings.language.clear();
            apply_now();
        }
        for (int index = 0; index < static_cast<int>(languages_.size()); ++index) {
            const auto& language = languages_[index];
            if (ImGui::Selectable(language.c_str(), selected_language == index + 1)) {
                settings.language = language;
                apply_now();
            }
        }
        ImGui::EndCombo();
    }
    pop_combo_style();
    form_row("自动识别游戏目录：");
    if (minecraft_checkbox(assets, "##auto-detect", "开启", &settings.auto_detect, false, book_text_color))
        apply_now();
    form_row("游戏目录：");
    ImGui::BeginDisabled(settings.auto_detect);
    const bool game_root_changed =
        textured_input(assets.widget("text_field.png"), assets.widget("text_field_highlighted.png"), "##game-root",
                       game_root_input_.data(), game_root_input_.size());
    if (game_root_changed) {
        settings.game_root = path_from_utf8(game_root_input_.data());
    }
    ImGui::EndDisabled();
    ImGui::EndTable();
    return changed || game_root_changed;
}

bool SettingsPanel::render_appearance(Settings& settings, const ImVec2 content_size, const float label_width,
                                      UiAssets& assets) {
    if (!ImGui::BeginTable("##appearance-settings", 2, ImGuiTableFlags_SizingStretchProp, content_size)) {
        return false;
    }
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, label_width);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch);
    bool changed = false;
    const auto checkbox = [&](const char* id, const char* label, bool* value, const bool keep_selected) {
        return minecraft_checkbox(assets, id, label, value, keep_selected, book_text_color);
    };
    const auto slider = [&](const char* id, float* value, const float min_value, const float max_value) {
        return textured_slider(assets.widget("slider.png"), assets.widget("slider_handle.png"),
                               assets.widget("slider_handle_highlighted.png"), id, value, min_value, max_value);
    };
    form_row("主窗口 缩放（倍率）：");
    changed = slider("##main-window-scale", &settings.main_window_scale, min_window_scale, max_window_scale) || changed;
    form_row("主窗口 背景色：");
    ImGui::PushStyleColor(ImGuiCol_Text, {1.0F, 1.0F, 1.0F, 1.0F});
    changed = ImGui::ColorEdit3("##main-background", settings.main_window_background_color.data()) || changed;
    ImGui::PopStyleColor();
    form_row("Overlay 显示：");
    changed = checkbox("##overlay-visible", "开启", &settings.overlay_visible, false) || changed;
    form_row("Overlay 透明背景：");
    changed = checkbox("##overlay-transparent", "开启", &settings.overlay_transparent, false) || changed;
    form_row("Overlay 滚动方向：");
    bool scroll_left = !settings.overlay_scroll_right;
    if (checkbox("##overlay-scroll-left", "向左", &scroll_left, true)) {
        settings.overlay_scroll_right = false;
        changed = true;
    }
    ImGui::SameLine();
    bool scroll_right = settings.overlay_scroll_right;
    if (checkbox("##overlay-scroll-right", "向右", &scroll_right, true)) {
        settings.overlay_scroll_right = true;
        changed = true;
    }
    form_row("Overlay 缩放（倍率）：");
    changed =
        slider("##overlay-window-scale", &settings.overlay_window_scale, min_window_scale, max_window_scale) || changed;
    form_row("Overlay 背景色：");
    ImGui::PushStyleColor(ImGuiCol_Text, {1.0F, 1.0F, 1.0F, 1.0F});
    changed = ImGui::ColorEdit3("##overlay-background", settings.overlay_window_background_color.data()) || changed;
    ImGui::PopStyleColor();
    form_row("Overlay 滚动速度：");
    changed = slider("##overlay-scroll-speed", &settings.overlay_scroll_speed, 0.0F, 300.0F) || changed;
    ImGui::EndTable();
    return changed;
}

void SettingsPanel::render_tips(const ImVec2 content_size) {
#if defined(__APPLE__)
    // ImGui reads Cmd as Ctrl on macOS, so clicks use Cmd; Ctrl+R and Cmd+R both restart.
    constexpr const char* click_key = "Cmd+点击图标";
    constexpr const char* restart_key = "Ctrl/Cmd+R";
#else
    constexpr const char* click_key = "Ctrl+点击图标";
    constexpr const char* restart_key = "Ctrl+R";
#endif
    const std::pair<const char*, const char*> tips[] = {
        {restart_key, "重开本轮：清除手动标记并重读存档"},
        {click_key, "左键手动完成，右键撤销"},
        {"拖动 Overlay", "移动位置；拖动边缘调整大小"},
        {"Esc", "关闭设置"},
    };
    // Tips are reference text, so pack the rows tighter than the form above.
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {ImGui::GetStyle().CellPadding.x * 3.0F, 1.0F});
    if (!ImGui::BeginTable("##tips", 2, ImGuiTableFlags_SizingFixedFit, content_size)) {
        ImGui::PopStyleVar();
        return;
    }
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch);
    for (const auto& [key, action] : tips) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(key);
        ImGui::TableSetColumnIndex(1);
        ImGui::TextDisabled("%s", action);
    }
    ImGui::EndTable();
    ImGui::PopStyleVar();
}

void SettingsPanel::render_actions(const ImVec2 padding, bool* apply, UiAssets& assets) {
    ImGui::SetCursorPosX(padding.x);
    ImGui::Separator();
    constexpr float action_width = 96.0F;
    const float action_height = ImGui::GetFrameHeight() + 8.0F;
    const float actions_width = (action_width * 2.0F) + ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), ImGui::GetWindowHeight() - padding.y - action_height));
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - actions_width) * 0.5F);
    ImGui::PushStyleColor(ImGuiCol_Text, {1.0F, 1.0F, 1.0F, 1.0F});
    if (minecraft_button(assets, "##close", "关闭", {action_width, action_height})) {
        close();
    }
    ImGui::SameLine();
    if (minecraft_button(assets, "##save", "保存", {action_width, action_height}) && apply != nullptr) {
        *apply = true;
    }
    ImGui::PopStyleColor();
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        close();
    }
}

}  // namespace beacon
