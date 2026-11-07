#pragma once

#include <beacon/core/model.hpp>
#include <beacon/io/file.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace beacon {

struct Localization {
    std::unordered_map<std::string, std::string> selected;
    std::unordered_map<std::string, std::string> defaults;

    [[nodiscard]] std::string_view text(std::string_view key) const;
};

// What a layout group shows.
enum class LayoutSource : std::uint8_t {
    // No items: a bare panel.
    None,
    // The listed rules.
    Nodes,
    // Every rule whose ID starts with one of the listed prefixes.
    Prefix,
    // The children of each listed rule, drawn as collections.
    Collections,
    // A board of the community leaderboard; main window only.
    Leaderboard,
};

struct ItemStyle {
    ItemEmphasis emphasis = ItemEmphasis::Highlight;
    ItemVisibility visibility = ItemVisibility::Visible;
    bool strikethrough = false;
    ItemAnimation animation = ItemAnimation::None;

    bool operator==(const ItemStyle&) const = default;
};

// Gaps around a group's items, in source icon pixels.
struct LayoutSpacing {
    // Horizontal gap between neighbouring items or collection columns.
    float item = 0.0F;
    // Vertical gap between rows; main window only, since the overlay draws one row.
    float row = 0.0F;
    // Gap between an icon frame and the label or collection title under it.
    float label = 0.0F;
    // Gap between the collections of a collections group.
    float collection = 0.0F;

    bool operator==(const LayoutSpacing&) const = default;
};
// The spacing a group gets for each field its layout leaves out.
LayoutSpacing default_spacing(bool overlay, LayoutSource source);
// Resolves a node's overrides against the defaults for this window and layout source.
ItemStyle resolve_item_style(const CompiledTemplate& compiled, std::uint32_t node, bool overlay, LayoutSource source,
                             bool done);

// A board of the community leaderboard: one tab of a public Google Sheet.
struct LeaderboardSource {
    // The spreadsheet's ID, from its URL.
    std::string sheet;
    // The tab's gid, from its URL.
    std::uint64_t gid = 0;
    // The board's title in the tab; empty for the tab's first board.
    std::string board;
    // The header's text; empty to show the board's title.
    std::string title;

    bool operator==(const LeaderboardSource&) const = default;
};

// A length that follows the window: a fraction of the window width or body height plus fixed source icon pixels.
struct LayoutLength {
    float fraction = 0.0F;
    float pixels = 0.0F;

    [[nodiscard]] float at(const float extent, const float scale) const {
        return (fraction * extent) + (pixels * scale);
    }
    LayoutLength operator+(const LayoutLength& other) const {
        return {fraction + other.fraction, pixels + other.pixels};
    }
    LayoutLength operator*(const float factor) const {
        return {fraction * factor, pixels * factor};
    }
    bool operator==(const LayoutLength&) const = default;
};

struct LayoutGroup {
    std::string id;
    // The panel's start and size along x and y, margins already taken off.
    std::array<LayoutLength, 2> position{};
    std::array<LayoutLength, 2> size{LayoutLength{1.0F, 0.0F}, LayoutLength{1.0F, 0.0F}};
    // Space inside the group's panel, in source icon pixels.
    float padding = 0.0F;
    LayoutSource source = LayoutSource::None;
    // Rule IDs, or ID prefixes for LayoutSource::Prefix.
    std::vector<std::string> nodes;
    std::vector<std::string> exclude;
    // LayoutSource::Leaderboard only.
    LeaderboardSource leaderboard;
    LayoutSpacing spacing{};
    // Overlay only: items roll through a carousel instead of sitting in a fixed strip.
    bool scroll = true;
    // Main window only: false inside a framed box, whose own frame-only group draws the panel instead.
    bool frame = true;

    bool operator==(const LayoutGroup&) const = default;
};

// Content panels in drawing order; boxes and springs are resolved during compilation. A framed box becomes an
// empty panel listed before its contents.
struct Layout {
    std::vector<LayoutGroup> main;
    std::vector<LayoutGroup> overlay;
    std::vector<Error> warnings;

    bool operator==(const Layout&) const = default;
};

struct TemplateResources {
    std::shared_ptr<const CompiledTemplate> compiled;
    std::shared_ptr<const Localization> localization;
    std::shared_ptr<const Layout> layout;
};

// The image files a node draws; paths it does not use are empty.
struct NodeImages {
    std::filesystem::path icon;
    std::filesystem::path frame_obtained;
    std::filesystem::path frame_unobtained;
    bool enchanted = false;
};
// Indexed by node, like CompiledTemplate::presentation_by_node.
std::vector<NodeImages> node_images(const CompiledTemplate& compiled);
using IconFiles = std::vector<std::pair<std::filesystem::path, FileStamp>>;
IconFiles icon_file_stamps(const CompiledTemplate& compiled);

class TemplateResourceLoader {
public:
    TemplateResourceLoader() = default;
    TemplateResourceLoader(std::filesystem::path template_path, std::filesystem::path asset_root,
                           TemplateResources resources);
    // A language-only change reuses validated rules, layout and images.
    ylt::expected<TemplateResources, Error> load(const std::filesystem::path& template_path,
                                                 const std::filesystem::path& asset_root, std::string_view language,
                                                 bool language_only = false);

private:
    std::filesystem::path template_path_;
    std::filesystem::path asset_root_;
    TemplateResources resources_;
    IconFiles icons_;
};

ylt::expected<Localization, Error> compile_localization_json(std::string_view default_json,
                                                             std::string_view selected_json = {});
ylt::expected<Layout, Error> compile_layout_json(std::string_view json, const CompiledTemplate& compiled);
std::vector<std::string> available_template_languages(const std::filesystem::path& template_path);
std::vector<std::uint32_t> resolve_layout_nodes(const LayoutGroup& group, const CompiledTemplate& compiled,
                                                std::size_t result_count);
std::string display_label(std::uint32_t node, const CompiledTemplate& compiled, const Localization& localization);
ylt::expected<void, Error> resolve_icon_paths(CompiledTemplate& compiled, const std::filesystem::path& template_root,
                                              const std::filesystem::path& asset_root);
ylt::expected<TemplateResources, Error> load_template_resources(const std::filesystem::path& template_path,
                                                                const std::filesystem::path& asset_root,
                                                                std::string_view language = {});

}  // namespace beacon
