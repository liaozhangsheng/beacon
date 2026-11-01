#include <beacon/ui/display.hpp>
#include <beacon/io/file.hpp>
#include <beacon/ui/sizing.hpp>
#include "../core/json.hpp"

#include <json/json.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>

namespace beacon {
namespace {

constexpr double max_layout_coordinate = 1'000'000.0;

Error resource_error(ErrorCode code, std::string message, std::string context) {
    return {.code = code, .message = std::move(message), .context = std::move(context)};
}

std::optional<Error> read_item_styles(const Json::Value& value, ItemStyles& styles, const std::string& path) {
    const auto invalid = [&] {
        return resource_error(ErrorCode::Validation, "invalid item_style", path);
    };
    if (!value.isObject())
        return invalid();
    for (const auto& state : value.getMemberNames()) {
        if (state != "pending" && state != "done")
            return invalid();
        const auto& fields = value[state];
        if (!fields.isObject())
            return invalid();
        auto& style = state == "done" ? styles.done : styles.pending;
        for (const auto& name : fields.getMemberNames()) {
            if (!fields[name].isString())
                return invalid();
            const auto setting = fields[name].asString();
            if (name == "emphasis") {
                if (setting == "highlight")
                    style.emphasis = ItemEmphasis::Highlight;
                else if (setting == "muted")
                    style.emphasis = ItemEmphasis::Muted;
                else
                    return invalid();
            } else if (name == "visibility") {
                if (setting != "visible" && setting != "blank")
                    return invalid();
                style.blank = setting == "blank";
            } else if (name == "decoration") {
                if (setting != "none" && setting != "strikethrough")
                    return invalid();
                style.strikethrough = setting == "strikethrough";
            } else {
                return invalid();
            }
        }
    }
    return std::nullopt;
}

// Reads a number in [0, max_layout_coordinate] into output; an absent field keeps output's default.
std::optional<Error> read_amount(const Json::Value& object, const std::string_view name, const std::string& path,
                                 float& output, const bool positive = false) {
    const std::string key(name);
    if (!object.isMember(key))
        return std::nullopt;
    const auto& value = object[key];
    if (!value.isNumeric() || !std::isfinite(value.asDouble()) || value.asDouble() < 0.0 ||
        (positive && value.asDouble() == 0.0) || value.asDouble() > max_layout_coordinate)
        return resource_error(ErrorCode::Validation, "layout value is outside its limit", child_path(path, name));
    output = static_cast<float>(value.asDouble());
    return std::nullopt;
}

std::optional<Error> read_spacing(const Json::Value& value, LayoutSpacing& spacing, const bool overlay,
                                  const LayoutSource source, const std::string& path) {
    if (!value.isObject())
        return resource_error(ErrorCode::Validation, "spacing must be an object", path);
    // Rows only exist in the main window, and collection gaps only between collections.
    const bool collections = source == LayoutSource::Collections;
    const auto failure = overlay ? collections ? reject_unknown_fields(value, path, {"item", "label", "collection"})
                                               : reject_unknown_fields(value, path, {"item", "label"})
                         : collections ? reject_unknown_fields(value, path, {"item", "row", "label", "collection"})
                                       : reject_unknown_fields(value, path, {"item", "row", "label"});
    if (failure)
        return failure;
    for (const auto& [name, field] :
         {std::pair{"item", &spacing.item}, std::pair{"row", &spacing.row}, std::pair{"label", &spacing.label},
          std::pair{"collection", &spacing.collection}}) {
        if (auto invalid = read_amount(value, name, path, *field))
            return invalid;
    }
    return std::nullopt;
}

ylt::expected<std::unordered_map<std::string, std::string>, Error> localization_map(std::string_view json,
                                                                                    std::string_view name) {
    auto root = beacon::parse_json(json, name, "invalid resource JSON");
    if (!root) {
        return ylt::unexpected<Error>{std::move(root.error())};
    }
    if (!root->isObject() || root->size() > max_rule_nodes) {
        return ylt::unexpected<Error>{
            resource_error(ErrorCode::Validation, "localization must be a bounded flat object", std::string(name))};
    }
    std::unordered_map<std::string, std::string> strings;
    strings.reserve(root->size());
    for (const auto& key : root->getMemberNames()) {
        const auto& value = (*root)[key];
        if (key.empty() || key.size() > max_string_bytes || !valid_utf8(key) || !value.isString()) {
            return ylt::unexpected<Error>{resource_error(ErrorCode::Validation, "invalid localization entry", key)};
        }
        const auto text = value.asString();
        if (text.size() > max_string_bytes || !valid_utf8(text)) {
            return ylt::unexpected<Error>{resource_error(ErrorCode::Validation, "invalid localization value", key)};
        }
        strings.emplace(key, text);
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
        return ylt::unexpected<Error>{resource_error(ErrorCode::Validation, "invalid layout root", root_path)};
    if (auto failure = reject_unknown_fields(*root, root_path, {"main", "overlay"}))
        return ylt::unexpected<Error>{std::move(*failure)};

    Layout result;
    std::size_t group_count = 0;
    const auto read_ids = [&](const Json::Value& ids, const std::string& path, std::vector<std::string>& output,
                              const bool require_rule) -> std::optional<Error> {
        if (!ids.isArray())
            return resource_error(ErrorCode::Validation, "expected an array of rule IDs", path);
        if (ids.size() > max_rule_nodes)
            return resource_error(ErrorCode::SecurityLimit, "layout group exceeds the node limit", path);
        for (Json::ArrayIndex index = 0; index < ids.size(); ++index) {
            const auto id_path = child_path(path, std::to_string(index));
            if (!ids[index].isString())
                return resource_error(ErrorCode::Validation, "layout group node must be a string", id_path);
            auto id = ids[index].asString();
            if (id.empty() || id.size() > max_string_bytes || !valid_utf8(id))
                return resource_error(ErrorCode::Validation, "invalid layout group node", id_path);
            if (require_rule && !compiled.graph.index_by_id.contains(id)) {
                if (result.warnings.size() < max_diagnostics) {
                    result.warnings.push_back(
                        resource_error(ErrorCode::Validation, "layout references an unknown rule ID", id));
                }
                continue;
            }
            output.push_back(std::move(id));
        }
        return std::nullopt;
    };
    const auto read_group = [&](const Json::Value& value, const std::string& path,
                                const bool overlay) -> ylt::expected<LayoutGroup, Error> {
        const auto fail = [](Error error) {
            return ylt::unexpected<Error>{std::move(error)};
        };
        if (!value.isObject())
            return fail(resource_error(ErrorCode::Validation, "layout group must be an object", path));
        if (auto failure =
                reject_unknown_fields(value, path,
                                      {"id", "x", "y", "width", "height", "margin", "padding", "nodes", "prefix",
                                       "collections", "children", "exclude", "spacing", "item_style", "scroll"}))
            return fail(std::move(*failure));

        LayoutGroup group;
        if (value.isMember("id")) {
            const auto& id = value["id"];
            if (!id.isString() || id.asString().size() > max_string_bytes || !valid_utf8(id.asString()))
                return fail(resource_error(ErrorCode::Validation, "invalid layout group id", child_path(path, "id")));
            group.id = id.asString();
        }
        for (const auto& [name, field, positive] :
             {std::tuple{"x", &group.x, false}, std::tuple{"y", &group.y, false},
              std::tuple{"width", &group.width, true}, std::tuple{"height", &group.height, true},
              std::tuple{"margin", &group.margin, false}, std::tuple{"padding", &group.padding, false}}) {
            if (auto failure = read_amount(value, name, path, *field, positive))
                return fail(std::move(*failure));
        }

        constexpr std::array selectors{std::pair{"nodes", LayoutSource::Nodes},
                                       std::pair{"prefix", LayoutSource::Prefix},
                                       std::pair{"collections", LayoutSource::Collections}};
        const auto contents = std::ranges::count_if(selectors,
                                                    [&](const auto& selector) {
                                                        return value.isMember(selector.first);
                                                    }) +
                              (value.isMember("children") ? 1 : 0);
        if (contents > 1)
            return fail(resource_error(ErrorCode::Validation,
                                       "a layout group takes only one of nodes, prefix, collections and children",
                                       path));
        for (const auto& [name, source] : selectors) {
            if (!value.isMember(name))
                continue;
            group.source = source;
            if (auto failure =
                    read_ids(value[name], child_path(path, name), group.nodes, source != LayoutSource::Prefix))
                return fail(std::move(*failure));
        }
        if (value.isMember("exclude")) {
            if (group.source == LayoutSource::None)
                return fail(resource_error(ErrorCode::Validation, "exclude requires nodes, prefix or collections",
                                           child_path(path, "exclude")));
            if (auto failure = read_ids(value["exclude"], child_path(path, "exclude"), group.exclude, true))
                return fail(std::move(*failure));
        }

        group.spacing = default_spacing(overlay, group.source);
        if (value.isMember("spacing")) {
            if (auto failure =
                    read_spacing(value["spacing"], group.spacing, overlay, group.source, child_path(path, "spacing")))
                return fail(std::move(*failure));
        }
        if (value.isMember("item_style")) {
            if (overlay || group.source != LayoutSource::Collections)
                return fail(resource_error(ErrorCode::Validation, "item_style requires main window collections",
                                           child_path(path, "item_style")));
            if (auto failure = read_item_styles(value["item_style"], group.item_style, child_path(path, "item_style")))
                return fail(std::move(*failure));
        }
        if (value.isMember("scroll")) {
            if (!overlay || !value["scroll"].isBool())
                return fail(resource_error(ErrorCode::Validation, "scroll requires an overlay boolean",
                                           child_path(path, "scroll")));
            group.scroll = value["scroll"].asBool();
        }
        return group;
    };
    // Every level is a grid: siblings are scaled together so their extent fills the parent's box.
    const auto compile_groups = [&](const auto& self, const Json::Value& values, const std::string& path,
                                    const bool overlay, const float parent_x, const float parent_y,
                                    const float parent_width, const float parent_height) -> std::optional<Error> {
        if (!values.isArray())
            return resource_error(ErrorCode::Validation, "expected an array of layout groups", path);
        std::vector<LayoutGroup> siblings;
        siblings.reserve(values.size());
        float extent_x = 0.0F;
        float extent_y = 0.0F;
        for (Json::ArrayIndex index = 0; index < values.size(); ++index) {
            if (++group_count > max_rule_nodes)
                return resource_error(ErrorCode::SecurityLimit, "layout exceeds the group limit", path);
            auto group = read_group(values[index], child_path(path, std::to_string(index)), overlay);
            if (!group)
                return std::move(group.error());
            extent_x = std::max(extent_x, group->x + group->width);
            extent_y = std::max(extent_y, group->y + group->height);
            siblings.push_back(std::move(*group));
        }
        auto& groups = overlay ? result.overlay : result.main;
        for (Json::ArrayIndex index = 0; index < values.size(); ++index) {
            auto& group = siblings[index];
            group.x = parent_x + (group.x / extent_x * parent_width);
            group.y = parent_y + (group.y / extent_y * parent_height);
            group.width = group.width / extent_x * parent_width;
            group.height = group.height / extent_y * parent_height;
            groups.push_back(group);
            if (values[index].isMember("children")) {
                if (auto failure = self(self, values[index]["children"],
                                        child_path(child_path(path, std::to_string(index)), "children"), overlay,
                                        group.x, group.y, group.width, group.height))
                    return failure;
            }
        }
        return std::nullopt;
    };
    for (const auto* window : {"main", "overlay"}) {
        if (!root->isMember(window))
            continue;
        if (auto failure = compile_groups(compile_groups, (*root)[window], child_path(root_path, window),
                                          std::string_view(window) == "overlay", 0.0F, 0.0F, 1.0F, 1.0F))
            return ylt::unexpected<Error>{std::move(*failure)};
    }
    return result;
}

ylt::expected<void, Error> resolve_icon_paths(CompiledTemplate& compiled, const std::filesystem::path& template_root,
                                              const std::filesystem::path& asset_root_input) {
    std::error_code ec;
    const auto absolute_root = std::filesystem::absolute(template_root, ec);
    if (ec) {
        return ylt::unexpected<Error>{resource_error(ErrorCode::Io, "cannot resolve template root", "template")};
    }
    const auto root = std::filesystem::weakly_canonical(absolute_root, ec);
    if (ec || !std::filesystem::is_directory(root, ec) || ec) {
        return ylt::unexpected<Error>{resource_error(ErrorCode::Io, "cannot resolve template root", "template")};
    }
    const auto absolute_asset_root = std::filesystem::absolute(asset_root_input, ec);
    if (ec) {
        return ylt::unexpected<Error>{resource_error(ErrorCode::Io, "cannot resolve asset root", "assets")};
    }
    const auto asset_root = std::filesystem::weakly_canonical(absolute_asset_root, ec);
    if (ec || !std::filesystem::is_directory(asset_root, ec) || ec) {
        return ylt::unexpected<Error>{resource_error(ErrorCode::Io, "cannot resolve asset root", "assets")};
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
        return ylt::unexpected<Error>{resource_error(ErrorCode::Validation, "invalid language", "language")};
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
                resource_error(ErrorCode::Internal, "template compiler returned no error", "template.json")};
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

ylt::expected<IconFiles, Error> icon_file_stamps(const CompiledTemplate& compiled) {
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
        auto stamp = file_stamp(path);
        if (!stamp)
            return ylt::unexpected<Error>{std::move(stamp.error())};
        files.emplace_back(std::move(path), *stamp);
    }
    return files;
}

TemplateResourceLoader::TemplateResourceLoader(std::filesystem::path template_path, std::filesystem::path asset_root,
                                               TemplateResources resources)
    : template_path_(std::move(template_path)), asset_root_(std::move(asset_root)), resources_(std::move(resources)) {
    if (resources_.compiled) {
        if (auto icons = icon_file_stamps(*resources_.compiled))
            icons_ = std::move(*icons);
    }
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
    if (!icons)
        return ylt::unexpected<Error>{std::move(icons.error())};
    if (same_package) {
        const auto& old = *resources_.compiled;
        const auto& next = *resources->compiled;
        if (old.same_rules(next) && old.template_name_key == next.template_name_key &&
            old.presentation_by_node == next.presentation_by_node && icons_ == *icons)
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
    icons_ = std::move(*icons);
    return resources;
}

}  // namespace beacon
