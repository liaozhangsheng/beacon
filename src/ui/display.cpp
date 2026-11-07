#include <beacon/ui/display.hpp>
#include <beacon/io/file.hpp>
#include <beacon/ui/sizing.hpp>
#include "../core/json.hpp"

#include <json/json.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <filesystem>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace beacon {
namespace {

constexpr double max_layout_coordinate = 1'000'000.0;

// One reader per field type: numbers stay in [0, max_layout_coordinate], text is never empty.
std::optional<Error> read_value(const Json::Value& value, const std::string& path, float& output) {
    if (!value.isNumeric() || !std::isfinite(value.asDouble()) || value.asDouble() < 0.0 ||
        value.asDouble() > max_layout_coordinate)
        return Error{ErrorCode::Validation, "layout value is outside its limit", path};
    output = static_cast<float>(value.asDouble());
    return std::nullopt;
}
std::optional<Error> read_value(const Json::Value& value, const std::string& path, bool& output) {
    if (!value.isBool())
        return Error{ErrorCode::Validation, "layout value must be a boolean", path};
    output = value.asBool();
    return std::nullopt;
}
std::optional<Error> read_value(const Json::Value& value, const std::string& path, std::string& output) {
    if (!value.isString() || value.asString().empty())
        return Error{ErrorCode::Validation, "layout value must be non-empty text", path};
    output = value.asString();
    return std::nullopt;
}
std::optional<Error> read_value(const Json::Value& value, const std::string& path, std::uint64_t& output) {
    if (!value.isUInt64())
        return Error{ErrorCode::Validation, "layout value must be a whole number", path};
    output = value.asUInt64();
    return std::nullopt;
}

template <class T>
concept JsonField = requires(const Json::Value& value, const std::string& path, T& output) {
    { read_value(value, path, output) } -> std::same_as<std::optional<Error>>;
};

// A JSON field bound to a member of Owner, read with the reader for the member's type.
template <class Owner, JsonField T> struct Field {
    const char* name;
    T Owner::* member;
};

// Reads each field the object has, in order, stopping at the first invalid one; absent fields keep their value.
template <class Owner, class... T>
std::optional<Error> read_fields(const Json::Value& object, const std::string& path, Owner& output,
                                 const Field<Owner, T>&... fields) {
    std::optional<Error> failure;
    (void)((object.isMember(fields.name) &&
            (failure = read_value(object[fields.name], child_path(path, fields.name), output.*fields.member))) ||
           ...);
    return failure;
}

// A standalone number; positive ones must also be above zero.
std::optional<Error> read_amount(const Json::Value& object, const char* name, const std::string& path, float& output,
                                 const bool positive = false) {
    if (!object.isMember(name))
        return std::nullopt;
    if (auto failure = read_value(object[name], child_path(path, name), output))
        return failure;
    if (positive && output == 0.0F)
        return Error{ErrorCode::Validation, "layout value is outside its limit", child_path(path, name)};
    return std::nullopt;
}

// Fields that exist in general but not for this window or group kind read as unknown.
std::optional<Error> reject_unless(const Json::Value& object, const std::string& path, const char* name,
                                   const bool allowed) {
    if (allowed || !object.isMember(name))
        return std::nullopt;
    return Error{ErrorCode::Validation, std::string("unknown field '") + name + "'", child_path(path, name)};
}

std::optional<Error> read_spacing(const Json::Value& value, LayoutSpacing& spacing, const bool overlay,
                                  const LayoutSource source, const std::string& path) {
    if (!value.isObject())
        return Error{ErrorCode::Validation, "spacing must be an object", path};
    // Rows only exist in the main window, and collection gaps only between collections.
    if (auto failure = reject_unknown_fields(value, path, {"item", "row", "label", "collection"}))
        return failure;
    if (auto failure = reject_unless(value, path, "row", !overlay))
        return failure;
    if (auto failure = reject_unless(value, path, "collection", source == LayoutSource::Collections))
        return failure;
    using S = LayoutSpacing;
    return read_fields(value, path, spacing, Field<S, float>{"item", &S::item}, Field<S, float>{"row", &S::row},
                       Field<S, float>{"label", &S::label}, Field<S, float>{"collection", &S::collection});
}

ylt::expected<std::unordered_map<std::string, std::string>, Error> localization_map(std::string_view json,
                                                                                    std::string_view name) {
    auto root = beacon::parse_json(json, name, "invalid resource JSON");
    if (!root) {
        return ylt::unexpected<Error>{std::move(root.error())};
    }
    if (!root->isObject()) {
        return ylt::unexpected<Error>{
            Error{ErrorCode::Validation, "localization must be a flat object", std::string(name)}};
    }
    std::unordered_map<std::string, std::string> strings;
    strings.reserve(root->size());
    for (const auto& key : root->getMemberNames()) {
        const auto& value = (*root)[key];
        if (key.empty() || !value.isString()) {
            return ylt::unexpected<Error>{Error{ErrorCode::Validation, "invalid localization entry", key}};
        }
        strings.emplace(key, value.asString());
    }
    return strings;
}

}  // namespace

std::string_view Localization::text(std::string_view key) const {
    if (const auto found = selected.find(std::string(key)); found != selected.end()) {
        return found->second;
    }
    if (const auto found = defaults.find(std::string(key)); found != defaults.end()) {
        return found->second;
    }
    return key;
}

ylt::expected<Localization, Error> compile_localization_json(std::string_view default_json,
                                                             std::string_view selected_json) {
    auto defaults = localization_map(default_json, "lang.json");
    if (!defaults) {
        return ylt::unexpected<Error>{std::move(defaults.error())};
    }
    Localization result;
    result.defaults = std::move(*defaults);
    if (!selected_json.empty()) {
        auto selected = localization_map(selected_json, "selected language");
        if (!selected) {
            return ylt::unexpected<Error>{std::move(selected.error())};
        }
        result.selected = std::move(*selected);
    }
    return result;
}

LayoutSpacing default_spacing(const bool overlay, const LayoutSource source) {
    const bool collections = source == LayoutSource::Collections;
    // Overlay collection icons sit one icon apart and their collections 48 apart; main collection entries
    // use 16-wide columns and a 32 row step for their 26-tall frames; labels hug their frames.
    if (overlay)
        return {.item = collections ? ui::source_icon_size : 0.0F, .label = 2.0F, .collection = 48.0F};
    return collections ? LayoutSpacing{.item = 16.0F, .row = 6.0F, .label = 3.0F} : LayoutSpacing{.label = 4.0F};
}

ylt::expected<Layout, Error> compile_layout_json(std::string_view json, const CompiledTemplate& compiled) {
    auto root = beacon::parse_json(json, "layout.json", "invalid resource JSON");
    if (!root) {
        return ylt::unexpected<Error>{std::move(root.error())};
    }
    const std::string root_path = "layout.json";
    if (!root->isObject())
        return ylt::unexpected<Error>{Error{ErrorCode::Validation, "invalid layout root", root_path}};
    if (auto failure = reject_unknown_fields(*root, root_path, {"main", "overlay"}))
        return ylt::unexpected<Error>{std::move(*failure)};

    Layout result;
    const auto read_ids = [&](const Json::Value& ids, const std::string& path, std::vector<std::string>& output,
                              const bool require_rule) -> std::optional<Error> {
        if (!ids.isArray())
            return Error{ErrorCode::Validation, "expected an array of rule IDs", path};
        for (Json::ArrayIndex index = 0; index < ids.size(); ++index) {
            const auto& id = ids[index];
            if (!id.isString() || id.asString().empty())
                return Error{ErrorCode::Validation, "invalid layout group node",
                             child_path(path, std::to_string(index))};
            if (require_rule && !compiled.graph.index_by_id.contains(id.asString())) {
                result.warnings.push_back(
                    Error{ErrorCode::Validation, "layout references an unknown rule ID", id.asString()});
                continue;
            }
            output.push_back(id.asString());
        }
        return std::nullopt;
    };
    const auto read_leaderboard = [](const Json::Value& board, const std::string& path,
                                     LeaderboardSource& output) -> std::optional<Error> {
        if (!board.isObject())
            return Error{ErrorCode::Validation, "invalid leaderboard", path};
        if (auto failure = reject_unknown_fields(board, path, {"sheet", "gid", "board", "title"}))
            return failure;
        // The ID goes into the download URL and the cache's file name, so it keeps to the characters Google uses.
        const auto& sheet = board["sheet"];
        if (!sheet.isString() || sheet.asString().empty() || sheet.asString().size() > 128 ||
            !std::ranges::all_of(sheet.asString(), [](const char c) {
                return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_';
            }))
            return Error{ErrorCode::Validation, "invalid leaderboard sheet", child_path(path, "sheet")};
        output.sheet = sheet.asString();
        using L = LeaderboardSource;
        return read_fields(board, path, output, Field<L, std::uint64_t>{"gid", &L::gid},
                           Field<L, std::string>{"board", &L::board}, Field<L, std::string>{"title", &L::title});
    };
    using G = LayoutGroup;
    constexpr Field<G, std::string> id_field{"id", &G::id};
    // A content panel; its rectangle comes from the boxes around it.
    const auto read_panel = [&](const Json::Value& value, const std::string& path, const bool overlay,
                                LayoutGroup& group) -> std::optional<Error> {
        if (auto failure = reject_unknown_fields(value, path,
                                                 {"id", "stretch", "size", "margin", "padding", "nodes", "prefix",
                                                  "collections", "leaderboard", "exclude", "spacing", "scroll"}))
            return failure;
        float margin = 0.0F;
        if (auto failure = read_fields(value, path, group, id_field, Field<G, float>{"padding", &G::padding}))
            return failure;
        if (auto failure = read_amount(value, "margin", path, margin))
            return failure;
        for (std::size_t axis = 0; axis < 2; ++axis) {
            group.position[axis].pixels += margin;
            group.size[axis].pixels -= margin * 2.0F;
        }

        constexpr std::array selectors{
            std::pair{"nodes", LayoutSource::Nodes}, std::pair{"prefix", LayoutSource::Prefix},
            std::pair{"collections", LayoutSource::Collections}, std::pair{"leaderboard", LayoutSource::Leaderboard}};
        for (const auto& [name, source] : selectors) {
            if (!value.isMember(name))
                continue;
            if (group.source != LayoutSource::None)
                return Error{ErrorCode::Validation,
                             "a layout group takes only one of nodes, prefix, collections and leaderboard", path};
            group.source = source;
            const auto field_path = child_path(path, name);
            if (source != LayoutSource::Leaderboard) {
                if (auto failure = read_ids(value[name], field_path, group.nodes, source != LayoutSource::Prefix))
                    return failure;
                continue;
            }
            if (overlay)
                return Error{ErrorCode::Validation, "leaderboard is main window only", field_path};
            if (auto failure = read_leaderboard(value[name], field_path, group.leaderboard))
                return failure;
        }
        const bool items = group.source != LayoutSource::None && group.source != LayoutSource::Leaderboard;
        for (const auto* field : {"exclude", "spacing"}) {
            if (value.isMember(field) && !items)
                return Error{ErrorCode::Validation, std::string(field) + " requires nodes, prefix or collections",
                             child_path(path, field)};
        }
        if (value.isMember("exclude")) {
            if (auto failure = read_ids(value["exclude"], child_path(path, "exclude"), group.exclude, true))
                return failure;
        }
        group.spacing = default_spacing(overlay, group.source);
        if (value.isMember("spacing")) {
            if (auto failure =
                    read_spacing(value["spacing"], group.spacing, overlay, group.source, child_path(path, "spacing")))
                return failure;
        }
        if (auto failure = reject_unless(value, path, "scroll", overlay))
            return failure;
        return read_fields(value, path, group, Field<G, bool>{"scroll", &G::scroll});
    };
    // Boxes give fixed-size children their size and divide the rest by stretch; springs keep their share empty.
    // Only panels, and the empty panel behind a framed box, reach the renderer.
    const auto compile_node = [&](const auto& self, const Json::Value& value, const std::string& path,
                                  const bool overlay, LayoutGroup rect) -> std::optional<Error> {
        if (!value.isObject())
            return Error{ErrorCode::Validation, "layout group must be an object", path};
        float amount = 1.0F;
        for (const auto* name : {"stretch", "size"}) {
            if (auto failure = read_amount(value, name, path, amount, true))
                return failure;
        }
        if (value.isMember("stretch") && value.isMember("size"))
            return Error{ErrorCode::Validation, "a layout child takes only one of stretch and size", path};
        const char* box = value.isMember("hbox") ? "hbox" : value.isMember("vbox") ? "vbox" : nullptr;
        if (box == nullptr && value.isMember("spring")) {
            if (auto failure = reject_unknown_fields(value, path, {"spring", "stretch", "size"}))
                return failure;
            if (!value["spring"].isBool() || !value["spring"].asBool())
                return Error{ErrorCode::Validation, "spring must be true", child_path(path, "spring")};
            return std::nullopt;
        }
        if (box == nullptr) {
            if (auto failure = read_panel(value, path, overlay, rect))
                return failure;
            (overlay ? result.overlay : result.main).push_back(std::move(rect));
            return std::nullopt;
        }

        if (auto failure = reject_unknown_fields(value, path, {"id", "stretch", "size", "gap", "frame", box}))
            return failure;
        // A framed box draws one panel over its whole rectangle, so springs inside it stay within the frame.
        bool framed = false;
        if (auto failure = reject_unless(value, path, "frame", !overlay))
            return failure;
        if (value.isMember("frame")) {
            if (auto failure = read_value(value["frame"], child_path(path, "frame"), framed))
                return failure;
            if (framed) {
                if (!rect.frame)
                    return Error{ErrorCode::Validation, "framed boxes cannot nest", child_path(path, "frame")};
                if (auto failure = read_fields(value, path, rect, id_field))
                    return failure;
                rect.spacing = default_spacing(false, LayoutSource::None);
                result.main.push_back(rect);
                rect.id.clear();
                rect.frame = false;
            }
        }
        float gap = 0.0F;
        if (auto failure = read_amount(value, "gap", path, gap))
            return failure;
        const auto box_path = child_path(path, box);
        const auto& children = value[box];
        if (!children.isArray())
            return Error{ErrorCode::Validation, "layout box must be an array", box_path};
        // A child takes its fixed size, or its stretch's share of what the gaps and fixed sizes leave. Positions
        // stay linear in the window size, so a fraction plus a fixed offset still describes each one exactly.
        std::vector<float> weights(children.size(), 1.0F);
        std::vector<float> sizes(children.size(), 0.0F);
        double total = 0.0;
        const auto axis = box == std::string_view("hbox") ? 0 : 1;
        auto free = rect.size[axis];
        free.pixels -= gap * static_cast<float>(children.empty() ? 0 : children.size() - 1);
        for (Json::ArrayIndex index = 0; index < children.size(); ++index) {
            // Malformed children are reported when they are compiled.
            if (children[index].isObject() && children[index].isMember("size")) {
                read_amount(children[index], "size", path, sizes[index], true);
                weights[index] = 0.0F;
            } else if (children[index].isObject()) {
                read_amount(children[index], "stretch", path, weights[index], true);
            }
            total += weights[index];
            free.pixels -= sizes[index];
        }
        auto start = rect.position[axis];
        for (Json::ArrayIndex index = 0; index < children.size(); ++index) {
            LayoutGroup child = rect;
            child.position[axis] = start;
            child.size[axis] = weights[index] > 0.0F ? free * static_cast<float>(weights[index] / total)
                                                     : LayoutLength{0.0F, sizes[index]};
            start = start + child.size[axis] + LayoutLength{0.0F, gap};
            if (auto failure = self(self, children[index], child_path(box_path, std::to_string(index)), overlay, child))
                return failure;
        }
        return std::nullopt;
    };
    for (const auto* window : {"main", "overlay"}) {
        if (!root->isMember(window))
            continue;
        if (auto failure = compile_node(compile_node, (*root)[window], child_path(root_path, window),
                                        std::string_view(window) == "overlay", LayoutGroup{}))
            return ylt::unexpected<Error>{std::move(*failure)};
    }
    return result;
}

ylt::expected<void, Error> resolve_icon_paths(CompiledTemplate& compiled, const std::filesystem::path& template_root,
                                              const std::filesystem::path& asset_root_input) {
    std::error_code ec;
    const auto absolute_root = std::filesystem::absolute(template_root, ec);
    if (ec) {
        return ylt::unexpected<Error>{Error{ErrorCode::Io, "cannot resolve template root", "template"}};
    }
    const auto root = std::filesystem::weakly_canonical(absolute_root, ec);
    if (ec || !std::filesystem::is_directory(root, ec) || ec) {
        return ylt::unexpected<Error>{Error{ErrorCode::Io, "cannot resolve template root", "template"}};
    }
    const auto absolute_asset_root = std::filesystem::absolute(asset_root_input, ec);
    if (ec) {
        return ylt::unexpected<Error>{Error{ErrorCode::Io, "cannot resolve asset root", "assets"}};
    }
    const auto asset_root = std::filesystem::weakly_canonical(absolute_asset_root, ec);
    if (ec || !std::filesystem::is_directory(asset_root, ec) || ec) {
        return ylt::unexpected<Error>{Error{ErrorCode::Io, "cannot resolve asset root", "assets"}};
    }
    const auto canonical_path = [&](const std::filesystem::path& base,
                                    const std::filesystem::path& relative) -> std::optional<std::filesystem::path> {
        ec.clear();
        const auto candidate = std::filesystem::weakly_canonical(base / relative, ec);
        if (ec || !path_is_within(base, candidate)) {
            return std::nullopt;
        }
        return candidate;
    };
    const auto regular_file = [&](const std::filesystem::path& path) {
        ec.clear();
        const auto result = std::filesystem::is_regular_file(path, ec);
        ec.clear();
        return result;
    };
    // Six shared frame files; resolve each only once during this resource load.
    std::unordered_map<std::string, std::string> frame_paths;
    for (auto& presentation : compiled.presentation_by_node) {
        if (!presentation) {
            continue;
        }
        const auto icon = valid_icon_reference(presentation->icon_path)
                              ? canonical_path(root, path_from_utf8(presentation->icon_path))
                              : std::nullopt;
        presentation->icon_path = icon && regular_file(*icon) ? path_to_utf8(*icon) : std::string{};

        if (presentation->frame_type == Presentation::FrameType::None) {
            continue;
        }
        const auto frame_name = presentation->frame_type == Presentation::FrameType::Goal        ? "goal_frame_"
                                : presentation->frame_type == Presentation::FrameType::Challenge ? "challenge_frame_"
                                                                                                 : "task_frame_";
        for (const auto& [state, output] : {std::pair{"obtained", &presentation->frame_obtained_path},
                                            std::pair{"unobtained", &presentation->frame_unobtained_path}}) {
            const auto filename = frame_name + std::string(state) + ".png";
            auto [cached, inserted] = frame_paths.try_emplace(filename);
            if (!inserted) {
                if (!cached->second.empty())
                    *output = cached->second;
                continue;
            }
            const auto frame = std::filesystem::weakly_canonical(asset_root / "assets/widgets" / filename, ec);
            if (ec || !path_is_within(asset_root, frame) || !std::filesystem::is_regular_file(frame, ec) || ec) {
                ec.clear();
                continue;
            }
            *output = cached->second = path_to_utf8(frame);
        }
    }
    return {};
}

std::vector<std::string> available_template_languages(const std::filesystem::path& template_path) {
    std::vector<std::string> result;
    std::error_code ec;
    const auto directory = template_path.parent_path() / "langs";
    for (std::filesystem::directory_iterator entries(directory, ec);
         !ec && entries != std::filesystem::directory_iterator{}; entries.increment(ec)) {
        if (!entries->is_regular_file(ec) || ec || entries->path().extension() != ".json") {
            ec.clear();
            continue;
        }
        const auto language = path_to_utf8(entries->path().stem());
        if (valid_language(language)) {
            result.push_back(language);
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

namespace {
ylt::expected<Localization, Error> load_localization(const std::filesystem::path& root, std::string_view language) {
    if (!language.empty() && !valid_language(language)) {
        return ylt::unexpected<Error>{Error{ErrorCode::Validation, "invalid language", "language"}};
    }
    auto default_language_json = read_bounded_file(root / "lang.json", "resource");
    if (!default_language_json) {
        return ylt::unexpected<Error>{std::move(default_language_json.error())};
    }
    std::string selected_language_json;
    if (!language.empty()) {
        auto selected_json = read_bounded_file(root / "langs" / (std::string(language) + ".json"), "resource");
        if (!selected_json) {
            return ylt::unexpected<Error>{std::move(selected_json.error())};
        }
        selected_language_json = std::move(*selected_json);
    }
    return compile_localization_json(*default_language_json, selected_language_json);
}
}  // namespace

ylt::expected<TemplateResources, Error> load_template_resources(const std::filesystem::path& template_path,
                                                                const std::filesystem::path& asset_root,
                                                                const std::string_view language) {
    const auto root = template_path.parent_path();
    auto template_json = read_bounded_file(template_path, "resource");
    if (!template_json) {
        return ylt::unexpected<Error>{std::move(template_json.error())};
    }
    auto compiled = compile_template_json(*template_json);
    if (!compiled) {
        if (compiled.error().empty()) {
            return ylt::unexpected<Error>{
                Error{ErrorCode::Internal, "template compiler returned no error", "template.json"}};
        }
        return ylt::unexpected<Error>{std::move(compiled.error().front())};
    }
    if (auto icons = resolve_icon_paths(*compiled, root, asset_root); !icons) {
        return ylt::unexpected<Error>{std::move(icons.error())};
    }
    auto localization = load_localization(root, language);
    if (!localization)
        return ylt::unexpected<Error>{std::move(localization.error())};
    auto layout_json = read_bounded_file(root / "layout.json", "resource");
    if (!layout_json) {
        return ylt::unexpected<Error>{std::move(layout_json.error())};
    }
    auto layout = compile_layout_json(*layout_json, *compiled);
    if (!layout) {
        return ylt::unexpected<Error>{std::move(layout.error())};
    }
    return TemplateResources{.compiled = std::make_shared<const CompiledTemplate>(std::move(*compiled)),
                             .localization = std::make_shared<const Localization>(std::move(*localization)),
                             .layout = std::make_shared<const Layout>(std::move(*layout))};
}

std::vector<NodeImages> node_images(const CompiledTemplate& compiled) {
    std::vector<NodeImages> images(compiled.presentation_by_node.size());
    for (std::size_t node = 0; node < images.size(); ++node) {
        if (const auto& view = compiled.presentation_by_node[node]) {
            images[node] = {.icon = path_from_utf8(view->icon_path),
                            .frame_obtained = path_from_utf8(view->frame_obtained_path),
                            .frame_unobtained = path_from_utf8(view->frame_unobtained_path),
                            .enchanted = view->enchanted};
        }
    }
    return images;
}

std::filesystem::path frame_glow_path(const std::filesystem::path& frame_obtained) {
    return frame_obtained.parent_path() / "frame_glow.png";
}

IconFiles icon_file_stamps(const CompiledTemplate& compiled) {
    std::vector<std::filesystem::path> paths;
    for (const auto& images : node_images(compiled)) {
        for (const auto* path : {&images.icon, &images.frame_obtained, &images.frame_unobtained}) {
            if (!path->empty())
                paths.push_back(*path);
        }
        if (!images.frame_obtained.empty())
            paths.push_back(frame_glow_path(images.frame_obtained));
    }
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    IconFiles files;
    for (auto& path : paths) {
        const auto stamp = file_stamp(path);
        files.emplace_back(std::move(path), stamp);
    }
    return files;
}

TemplateResourceLoader::TemplateResourceLoader(std::filesystem::path template_path, std::filesystem::path asset_root,
                                               TemplateResources resources)
    : template_path_(std::move(template_path)), asset_root_(std::move(asset_root)), resources_(std::move(resources)) {
    if (resources_.compiled)
        icons_ = icon_file_stamps(*resources_.compiled);
}

ylt::expected<TemplateResources, Error> TemplateResourceLoader::load(const std::filesystem::path& template_path,
                                                                     const std::filesystem::path& asset_root,
                                                                     std::string_view language, bool language_only) {
    const bool same_package = resources_.compiled && template_path_ == template_path && asset_root_ == asset_root;
    if (same_package && language_only) {
        auto localization = load_localization(template_path.parent_path(), language);
        if (!localization)
            return ylt::unexpected<Error>{std::move(localization.error())};
        resources_.localization = std::make_shared<const Localization>(std::move(*localization));
        return resources_;
    }
    auto resources = load_template_resources(template_path, asset_root, language);
    if (!resources)
        return resources;
    auto icons = icon_file_stamps(*resources->compiled);
    if (same_package) {
        const auto& old = *resources_.compiled;
        const auto& next = *resources->compiled;
        if (old.same_rules(next) && old.template_name_key == next.template_name_key &&
            old.presentation_by_node == next.presentation_by_node && icons_ == icons)
            resources->compiled = resources_.compiled;
        if (*resources->layout == *resources_.layout)
            resources->layout = resources_.layout;
        if (resources_.localization->defaults == resources->localization->defaults &&
            resources_.localization->selected == resources->localization->selected)
            resources->localization = resources_.localization;
    }
    template_path_ = template_path;
    asset_root_ = asset_root;
    resources_ = *resources;
    icons_ = std::move(icons);
    return resources;
}

}  // namespace beacon
