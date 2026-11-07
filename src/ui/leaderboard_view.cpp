#include "leaderboard_view.hpp"

#include <beacon/ui/language.hpp>
#include <beacon/ui/sizing.hpp>

#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace beacon {
namespace {

constexpr ImU32 text_color = IM_COL32(245, 245, 245, 255);
constexpr ImU32 muted_color = IM_COL32(150, 150, 150, 255);
constexpr ImU32 igt_color = IM_COL32(255, 215, 0, 255);
// §c, for an update that could not reach the sheet.
constexpr ImU32 failed_color = IM_COL32(255, 85, 85, 255);
// advancement.png, the toast the completion view also uses.
constexpr NineSlice toast_slice{4.0F, 4.0F, 4.0F, 4.0F};

// Gold, silver and bronze badges are cut from the matching block and glow; everyone else gets a grey bevel.
struct Placing {
    ImU32 metal;
    ImU32 light;
    ImU32 shadow;
    ImU32 panel;
    ImU32 label;
    const char* block = nullptr;
};

constexpr std::array<Placing, 4> placings{{
    {IM_COL32(240, 190, 50, 255), IM_COL32(255, 238, 140, 255), IM_COL32(168, 110, 20, 255), IM_COL32(96, 64, 20, 255),
     IM_COL32(255, 246, 210, 255), "leaderboard/gold_block.png"},
    {IM_COL32(190, 198, 214, 255), IM_COL32(240, 244, 250, 255), IM_COL32(108, 116, 134, 255),
     IM_COL32(66, 72, 86, 255), IM_COL32_WHITE, "leaderboard/iron_block.png"},
    {IM_COL32(214, 128, 74, 255), IM_COL32(248, 186, 134, 255), IM_COL32(138, 70, 38, 255), IM_COL32(92, 48, 34, 255),
     IM_COL32(255, 226, 200, 255), "leaderboard/copper_block.png"},
    {IM_COL32(88, 88, 96, 255), IM_COL32(122, 122, 132, 255), IM_COL32(52, 52, 58, 255), IM_COL32(38, 38, 44, 255),
     IM_COL32_WHITE},
}};

// A missing or unreadable rank is 0, styled like the runs past the podium.
const Placing& placing(const int rank) {
    return placings[static_cast<std::size_t>((rank < 1 ? 4 : std::min(rank, 4)) - 1)];
}

std::string rank_label(const int rank) {
    if (rank < 1)
        return "-";
    if (rank == 1)
        return "WR";
    const int tens = rank % 100;
    const int ones = rank % 10;
    const char* suffix = (tens >= 11 && tens <= 13) || ones == 0 || ones > 3 ? "th"
                         : ones == 1                                         ? "st"
                         : ones == 2                                         ? "nd"
                                                                             : "rd";
    return std::to_string(rank) + suffix;
}

// A slanted band of light that crosses [min, max] in the first 40% of every three seconds.
void draw_glint(ImDrawList* draw, const ImVec2 min, const ImVec2 max, const double time) {
    const float height = max.y - min.y;
    const float band = height * 0.5F;
    const float progress = std::min(static_cast<float>(std::fmod(time / 3.0, 1.0)) / 0.4F, 1.0F);
    const float center = min.x - band - height + (progress * ((max.x - min.x) + (2.0F * (band + height))));
    const ImVec2 uv = draw->_Data->TexUvWhitePixel;
    // Three columns (edge, centre, edge), each leaning right by the height, joined by two quads.
    // PrimReserve may start a new vertex offset, so the first index is only known after it.
    draw->PrimReserve(12, 6);
    const auto index = static_cast<ImDrawIdx>(draw->_VtxCurrentIdx);
    for (const auto [x, alpha] : {std::pair{center - band, 0}, {center, 150}, {center + band, 0}}) {
        draw->PrimWriteVtx({x + height, min.y}, uv, IM_COL32(255, 255, 255, alpha));
        draw->PrimWriteVtx({x, max.y}, uv, IM_COL32(255, 255, 255, alpha));
    }
    for (const int column : {0, 2}) {
        for (const int corner : {0, 1, 3, 0, 3, 2})
            draw->PrimWriteIdx(static_cast<ImDrawIdx>(index + column + corner));
    }
}

// A plaque on a grid of `pixel` squares: a black outline, a metal rim (the block texture, or flat metal) and a
// dark panel holding the label.
void draw_badge(ImDrawList* draw, const ImVec2 min, const ImVec2 size, const float pixel, const Placing& style,
                SDL_Texture* block, const std::string& label) {
    const ImVec2 max{min.x + size.x, min.y + size.y};
    const auto rect = [&](const float inset_min, const float inset_max, const ImU32 color) {
        draw->AddRectFilled({min.x + (inset_min * pixel), min.y + (inset_min * pixel)},
                            {max.x - (inset_max * pixel), max.y - (inset_max * pixel)}, color);
    };
    rect(0, 0, IM_COL32_BLACK);
    rect(1, 1, style.metal);
    if (block != nullptr) {
        // Whole 16-texel tiles, one texel per badge pixel, cut off at the rim.
        const float tile = pixel * 16.0F;
        draw->PushClipRect({min.x + pixel, min.y + pixel}, {max.x - pixel, max.y - pixel}, true);
        for (float y = min.y + pixel; y < max.y; y += tile) {
            for (float x = min.x + pixel; x < max.x; x += tile)
                draw->AddImage(block, {x, y}, {x + tile, y + tile});
        }
        draw->PopClipRect();
    }
    rect(2, 2, style.panel);
    const ImVec2 label_size = ImGui::CalcTextSize(label.c_str());
    draw_shadowed_text(
        draw,
        {std::round(min.x + ((size.x - label_size.x) * 0.5F)), std::round(min.y + ((size.y - label_size.y) * 0.5F))},
        style.label, label.c_str());
}

// A run's proportions in the current font. Heads and badges snap to whole screen pixels so their pixel art stays crisp.
// The rank badge overlaps the head's top-left corner; then name over date, and the IGT on the right.
struct RowMetrics {
    float unit = ImGui::GetFontSize();
    float pixel = std::max(1.0F, std::round(unit / 8.0F));
    float pad = pixel * 4.0F;
    float gap = std::max(2.0F, std::round(unit * 0.75F));
    // A whole number of screen pixels per skin pixel, tall enough for the two text lines beside it.
    float head_size = 8.0F * std::max(3.0F, std::round(unit * 3.0F / 8.0F));
    float frame_pixel = head_size / 32.0F;
    float frame = frame_pixel * 3.0F;
    float badge_height = ImGui::GetTextLineHeight() + (pixel * 6.0F);
    float badge_min_width = pixel * 20.0F;
    // The head sits under the middle of the narrowest badge, so heads line up whatever the label.
    float head_x = pad + (badge_min_width * 0.5F);
    float head_y = pad + (badge_height * 0.5F);
    float text_x = head_x + head_size + frame + gap;
    // As much space under the head as above it.
    float height = head_y + head_size + head_y;
};

void draw_run(const LeaderboardRun& run, SDL_Texture* head, SDL_Texture* glow, UiAssets& assets, const double time) {
    auto* draw = ImGui::GetWindowDrawList();
    const RowMetrics m;
    const Placing& style = placing(run.rank);
    const bool podium = style.block != nullptr;
    const std::string label = rank_label(run.rank);

    const ImVec2 row_min = ImGui::GetCursorScreenPos();
    const ImVec2 row_max{row_min.x + ImGui::GetContentRegionAvail().x, row_min.y + m.height};
    // Runs with a link open it when clicked.
    ImGui::InvisibleButton("##run", {row_max.x - row_min.x, row_max.y - row_min.y});
    const bool linked = !run.link.empty();
    const bool hovered = linked && ImGui::IsItemHovered();
    if (hovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        minecraft_tooltip(run.link.c_str());
    }
    if (linked && ImGui::IsItemClicked(ImGuiMouseButton_Left))
        SDL_OpenURL(run.link.c_str());
    const ImVec2 badge_size{std::max(m.badge_min_width, ImGui::CalcTextSize(label.c_str()).x + (m.pixel * 6.0F)),
                            m.badge_height};
    const ImVec2 badge_min{row_min.x + m.pad, row_min.y + m.pad};
    const ImVec2 badge_max{badge_min.x + badge_size.x, badge_min.y + badge_size.y};
    const ImVec2 head_min{row_min.x + m.head_x, row_min.y + m.head_y};
    const ImVec2 head_max{head_min.x + m.head_size, head_min.y + m.head_size};
    const ImVec2 head_center{(head_min.x + head_max.x) * 0.5F, (head_min.y + head_max.y) * 0.5F};
    const float phase = static_cast<float>(run.rank) * 2.39996323F;

    draw_nine_slice(draw, assets.widget("advancement.png"), row_min, row_max, toast_slice, m.pixel);
    const ImVec2 inner_min{row_min.x + (m.pixel * 2.0F), row_min.y + (m.pixel * 2.0F)};
    const ImVec2 inner_max{row_max.x - (m.pixel * 2.0F), row_max.y - (m.pixel * 2.0F)};
    // Podium rows fade in from their metal on the left.
    if (podium) {
        const ImU32 tint = (style.metal & ~IM_COL32_A_MASK) | IM_COL32(0, 0, 0, 40);
        const ImU32 clear = style.metal & ~IM_COL32_A_MASK;
        draw->AddRectFilledMultiColor(inner_min, {inner_min.x + ((inner_max.x - inner_min.x) * 0.6F), inner_max.y},
                                      tint, clear, clear, tint);
    }
    if (hovered)
        draw->AddRectFilled(inner_min, inner_max, IM_COL32(255, 255, 255, ImGui::IsItemActive() ? 10 : 22));
    draw->PushClipRect(inner_min, inner_max, true);
    if (podium) {
        // A breathing halo in the placing's metal, stacked rings fading outwards, under the rotating glow.
        const float breath = 0.5F + (0.5F * static_cast<float>(std::sin((time * 2.4) + phase)));
        const float pulse = 0.5F + (0.5F * breath);
        // Its reach past the head breathes too, drawing in close at the low point.
        const float reach = 0.4F * (0.4F + (0.6F * breath));
        const ImU32 metal = style.metal & ~IM_COL32_A_MASK;
        constexpr int rings = 8;
        for (int i = 0; i < rings; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(rings - 1);
            // Out to just past the frame at full breath, short of the name and date beside the head.
            const float radius = m.head_size * (0.55F + (reach * (1.0F - t)));
            draw->AddCircleFilled(head_center, radius, metal | IM_COL32(0, 0, 0, static_cast<int>(22.0F * pulse)));
        }
    }
    // Podium heads and badges glow in their placing's metal. The head's glow fades out before the text beside it.
    if (podium && glow != nullptr) {
        // Two layers turning against each other, so the glow reads past the frame.
        draw_rotating_glow(draw, glow, head_center, m.head_size * 1.35F, time, phase, 1.7F, style.light);
        draw_rotating_glow(draw, glow, head_center, m.head_size * 1.1F, -time, phase + 0.8F, 1.7F, style.light);
        draw_rotating_glow(draw, glow, {(badge_min.x + badge_max.x) * 0.5F, (badge_min.y + badge_max.y) * 0.5F},
                           m.badge_height * 1.5F, time, phase + 1.0F, 1.0F, style.light);
    }
    if (head == nullptr)
        head = assets.image("default-avatar.png");
    draw->AddRectFilled({head_min.x - m.frame, head_min.y - m.frame}, {head_max.x + m.frame, head_max.y + m.frame},
                        podium ? style.metal : IM_COL32(78, 81, 86, 255));
    draw->AddRectFilled({head_min.x - m.frame_pixel, head_min.y - m.frame_pixel},
                        {head_max.x + m.frame_pixel, head_max.y + m.frame_pixel}, IM_COL32(30, 30, 34, 255));
    if (head != nullptr)
        draw->AddImage(head, head_min, head_max);
    draw_badge(draw, badge_min, badge_size, m.pixel, style, podium ? assets.image(style.block) : nullptr, label);
    if (podium) {
        draw->PushClipRect({badge_min.x + m.pixel, badge_min.y + m.pixel},
                           {badge_max.x - m.pixel, badge_max.y - m.pixel}, true);
        draw_glint(draw, badge_min, badge_max, time + (run.rank * 0.3));
        draw->PopClipRect();
    }

    const float line = ImGui::GetTextLineHeight();
    const float center_y = std::round(head_center.y);
    const float igt_x = row_max.x - (m.pad * 2.0F) - ImGui::CalcTextSize(run.igt.c_str()).x;
    draw_shadowed_text(draw, {igt_x, std::round(center_y - (line * 0.5F))}, igt_color, run.igt.c_str());
    const float text_x = row_min.x + m.text_x;
    const float lines_gap = std::max(1.0F, std::round(m.pixel * 2.0F));
    const float name_y = std::round(center_y - line - (lines_gap * 0.5F));
    draw_shadowed_text(draw, {text_x, name_y}, podium ? style.light : text_color,
                       ellipsize_text(run.runner, igt_x - m.gap - text_x).c_str());
    draw_shadowed_text(draw, {text_x, name_y + line + lines_gap}, muted_color,
                       ellipsize_text(run.date, igt_x - m.gap - text_x).c_str());
    draw->PopClipRect();
}

// A clockwise arrow drawn on a 7x7 grid, centred in the square at min.
void draw_refresh_icon(ImDrawList* draw, const ImVec2 min, const float size, const ImU32 color) {
    constexpr std::array<std::string_view, 7> rows{
        "..###.#", ".#...##", "#...###", "#......", "#.....#", ".#...#.", "..###..",
    };
    const float cell = std::max(1.0F, std::floor(size * 0.55F / 7.0F));
    const float origin_x = std::round(min.x + ((size - (cell * 7.0F)) * 0.5F));
    const float origin_y = std::round(min.y + ((size - (cell * 7.0F)) * 0.5F));
    // The shadow first, one square down and right, as the game shades text.
    for (const auto [offset, tint] : {std::pair{cell, IM_COL32(63, 63, 63, 255)}, std::pair{0.0F, color}}) {
        for (std::size_t y = 0; y < rows.size(); ++y) {
            for (std::size_t x = 0; x < rows[y].size(); ++x) {
                if (rows[y][x] != '#')
                    continue;
                const ImVec2 square{origin_x + offset + (static_cast<float>(x) * cell),
                                    origin_y + offset + (static_cast<float>(y) * cell)};
                draw->AddRectFilled(square, {square.x + cell, square.y + cell}, tint);
            }
        }
    }
}

// Shown in place of a board that cannot be drawn yet, or at all.
const char* missing_text(const LeaderboardStatus status, const std::string_view language) {
    switch (status) {
        case LeaderboardStatus::Ready:
        case LeaderboardStatus::Offline:
            return interface_text(language, "排行榜中没有此榜单", "This board is not on the leaderboard");
        case LeaderboardStatus::Failed:
            return interface_text(language, "无法加载排行榜，点击右上角按钮重试",
                                  "Could not load the leaderboard; retry with the button above");
        default:
            return interface_text(language, "正在加载排行榜...", "Loading leaderboard...");
    }
}

}  // namespace

void LeaderboardView::draw(const ImVec2 min, const ImVec2 max, const LeaderboardSource& source,
                           LeaderboardLoader& loader, SDL_Texture* glow, UiAssets& assets, SDL_Renderer* renderer,
                           const std::string_view language, FrameSchedule& schedule) {
    loader.start();
    auto update = loader.take();
    for (auto& [name, skin] : update.skins)
        heads_[name] = skin_head_texture(renderer, skin);
    const auto& boards = *update.boards;
    const auto board =
        source.board.empty() ? boards.begin() : std::ranges::find(boards, source.board, &LeaderboardBoard::title);
    const std::string& title = !source.title.empty()   ? source.title
                               : board != boards.end() ? board->title
                                                       : source.board;
    const float unit = ImGui::GetFontSize();
    const float pixel = std::max(1.0F, std::round(unit / 8.0F));
    auto* draw = ImGui::GetWindowDrawList();

    // Header: a toast like the runs, with the board's version range and a button to download the sheet again.
    const bool busy = update.status == LeaderboardStatus::Loading || update.status == LeaderboardStatus::Updating;
    // The version is set twice the size of the runs, centred, with the WR badge's glint, so it reads at a glance.
    ImGui::PushFont(nullptr, std::round(unit * 2.0F));
    const float line = ImGui::GetTextLineHeight();
    const float bar_height = std::round(line + (pixel * 10.0F));
    const ImVec2 bar_max{max.x, min.y + bar_height};
    draw_nine_slice(draw, assets.widget("advancement.png"), min, bar_max, toast_slice, pixel);
    // The button sits inside the toast's border, as far from it on every side.
    const float border = toast_slice.left * pixel;
    const float button_size = bar_height - (border * 2.0F);
    const ImVec2 button_min{bar_max.x - border - button_size, min.y + border};
    const float title_min = min.x + border;
    const float title_max = button_min.x - pixel;
    const std::string label = ellipsize_text(title, title_max - title_min);
    const float title_x =
        std::round(title_min + ((title_max - title_min - ImGui::CalcTextSize(label.c_str()).x) * 0.5F));
    draw->PushClipRect({title_min, min.y + border}, {title_max, bar_max.y - border}, true);
    draw_shadowed_text(draw, {title_x, std::round(min.y + ((bar_height - line) * 0.5F))}, igt_color, label.c_str());
    draw_glint(draw, {title_min, min.y + border}, {title_max, bar_max.y - border}, ImGui::GetTime() + 1.5);
    draw->PopClipRect();
    ImGui::PopFont();
    ImGui::SetCursorScreenPos(button_min);
    ImGui::BeginDisabled(busy);
    if (textured_button(assets.widget(busy ? "button_disabled.png" : "button.png"),
                        assets.widget("button_highlighted.png"), "##leaderboard-update", "",
                        {button_size, button_size}))
        loader.refresh();
    ImGui::EndDisabled();
    const bool offline = update.status == LeaderboardStatus::Offline || update.status == LeaderboardStatus::Failed;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        std::string tooltip;
        if (busy)
            tooltip = interface_text(language, "正在更新排行榜...", "Updating the leaderboard...");
        else if (offline)
            tooltip =
                interface_text(language, "无法连接排行榜，点击重试", "Could not reach the leaderboard; click to retry");
        else
            tooltip = interface_text(language, "更新排行榜", "Update the leaderboard");
        if (!update.downloaded.empty())
            tooltip += interface_text(language, "\n下载于 ", "\nDownloaded ") + update.downloaded;
        minecraft_tooltip(tooltip.c_str());
    }
    draw_refresh_icon(draw, button_min, button_size, busy ? muted_color : offline ? failed_color : text_color);
    // The glints and the podium glows move.
    schedule.every(soft_animation_ms);
    const ImVec2 list_min{min.x, bar_max.y + std::max(1.0F, std::round(unit * 0.25F))};

    if (board == boards.end()) {
        const std::string message = ellipsize_text(missing_text(update.status, language), max.x - min.x);
        const ImVec2 size = ImGui::CalcTextSize(message.c_str());
        draw->PushClipRect(list_min, max, true);
        draw_shadowed_text(draw,
                           {std::round(min.x + ((max.x - min.x - size.x) * 0.5F)),
                            std::round(list_min.y + ((max.y - list_min.y - size.y) * 0.5F))},
                           muted_color, message.c_str());
        draw->PopClipRect();
        return;
    }

    ImGui::SetCursorScreenPos(list_min);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, std::max(3.0F, std::round(unit * 0.5F)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {0.0F, std::max(1.0F, std::round(unit * 0.25F))});
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, IM_COL32_BLACK_TRANS);
    if (ImGui::BeginChild("##leaderboard", {max.x - list_min.x, std::max(1.0F, max.y - list_min.y)},
                          ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground)) {
        const double time = ImGui::GetTime();
        // Rows share one height, so only those in view are drawn; a board can hold hundreds of runs.
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(board->runs.size()), RowMetrics{}.height + ImGui::GetStyle().ItemSpacing.y);
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const auto& run = board->runs[static_cast<std::size_t>(i)];
                const auto head = heads_.find(run.runner);
                ImGui::PushID(i);
                draw_run(run, head == heads_.end() ? nullptr : head->second.get(), glow, assets, time);
                ImGui::PopID();
            }
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

}  // namespace beacon
