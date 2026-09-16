#include <beacon/ui/display.hpp>
#include <beacon/io/file.hpp>
#include "../core/json.hpp"

#include <json/json.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <array>
#include <memory>
#include <optional>
#include <string>

namespace beacon {
namespace {

constexpr double max_layout_coordinate = 1'000'000.0;

bool path_is_within(const std::filesystem::path& root, const std::filesystem::path& candidate) {
    return std::mismatch(root.begin(), root.end(), candidate.begin(), candidate.end()).first == root.end();
}

Error resource_error(ErrorCode code, std::string message, std::string context) {
    return {.code = code, .message = std::move(message), .context = std::move(context)};
}

bool exact_layout_group_fields(const Json::Value& value) {
    constexpr std::array<std::string_view, 10> fields{"id", "source", "nodes",  "exclude", "x",
                                                      "y",  "width",  "height", "margin",  "padding"};
    if (!value.isObject() || (value.size() != fields.size() && value.size() != fields.size() + 1)) {
        return false;
    }
    for (const auto field : fields) {
        if (!value.isMember(std::string(field))) {
            return false;
        }
    }
    return value.size() == fields.size() + (value.isMember("children") ? 1 : 0);
}

void normalize_layout_groups(std::vector<LayoutGroup>& groups) {
    float max_right = 0.0F;
    float max_bottom = 0.0F;
    for (const auto& group : groups) {
        max_right = std::max(max_right, group.x + group.width);
        max_bottom = std::max(max_bottom, group.y + group.height);
    }
    for (auto& group : groups) {
        group.x /= max_right;
        group.y /= max_bottom;
        group.width /= max_right;
        group.height /= max_bottom;
    }
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

ylt::expected<Layout, Error> compile_layout_json(std::string_view json, const CompiledTemplate& compiled) {
    auto root = beacon::parse_json(json, "layout.json", "invalid resource JSON");
    if (!root) {
        return ylt::unexpected<Error>{std::move(root.error())};
    }
    if (!root->isObject() || !exact_json_fields(*root, {"main", "overlay"}) || !(*root)["main"].isArray() ||
        !(*root)["overlay"].isArray() || (*root)["main"].size() + (*root)["overlay"].size() > max_rule_nodes) {
        return ylt::unexpected<Error>{resource_error(ErrorCode::Validation, "invalid layout root", "layout.json")};
    }

    Layout result;
    std::size_t group_count = 0;
    const auto compile_groups = [&](const auto& self, const Json::Value& values, std::vector<LayoutGroup>& groups,
                                    const double parent_x, const double parent_y, const double parent_width,
                                    const double parent_height, const bool normalize_children) -> std::optional<Error> {
        double max_right = 1.0;
        double max_bottom = 1.0;
        if (normalize_children) {
            for (const auto& value : values) {
                if (value.isObject() && value["x"].isNumeric() && value["width"].isNumeric() &&
                    value["y"].isNumeric() && value["height"].isNumeric()) {
                    max_right = std::max(max_right, value["x"].asDouble() + value["width"].asDouble());
                    max_bottom = std::max(max_bottom, value["y"].asDouble() + value["height"].asDouble());
                }
            }
        }
        for (Json::ArrayIndex index = 0; index < values.size(); ++index) {
            if (++group_count > max_rule_nodes) {
                return resource_error(ErrorCode::SecurityLimit, "layout exceeds the group limit", "layout.json");
            }
            const auto& value = values[index];
            const auto valid_fields = exact_layout_group_fields(value);
            if (!valid_fields || !value["source"].isString() || !value["nodes"].isArray() ||
                !value["exclude"].isArray() || !value["x"].isNumeric() || !value["y"].isNumeric() ||
                !value["width"].isNumeric() || !value["height"].isNumeric() || !value["id"].isString() ||
                !value["margin"].isNumeric() || !value["padding"].isNumeric()) {
                return resource_error(ErrorCode::Validation, "invalid layout group", std::to_string(index));
            }
            const auto x = parent_x + value["x"].asDouble() / max_right * parent_width;
            const auto y = parent_y + value["y"].asDouble() / max_bottom * parent_height;
            const auto width = value["width"].asDouble() / max_right * parent_width;
            const auto height = value["height"].asDouble() / max_bottom * parent_height;
            const auto margin = value["margin"].asDouble();
            const auto padding = value["padding"].asDouble();
            const auto id = value["id"].asString();
            const auto source_name = value["source"].asString();
            if (id.empty() || id.size() > max_string_bytes || !valid_utf8(id) ||
                source_name.size() > max_string_bytes || !valid_utf8(source_name) || !std::isfinite(x) ||
                !std::isfinite(y) || !std::isfinite(width) || !std::isfinite(height) || !std::isfinite(margin) ||
                !std::isfinite(padding) || margin < 0 || padding < 0 || margin > max_layout_coordinate ||
                padding > max_layout_coordinate || x < 0 || y < 0 || width <= 0 || height <= 0 ||
                x > max_layout_coordinate - width || y > max_layout_coordinate - height) {
                return resource_error(ErrorCode::Validation, "layout group value is outside its limit",
                                      std::to_string(index));
            }
            LayoutSource source;
            if (source_name == "nodes")
                source = LayoutSource::Nodes;
            else if (source_name == "prefix")
                source = LayoutSource::Prefix;
            else if (source_name == "children")
                source = LayoutSource::Children;
            else
                return resource_error(ErrorCode::Validation, "unknown layout source", source_name);

            LayoutGroup group{.id = id,
                              .source = source,
                              .nodes = {},
                              .exclude = {},
                              .x = static_cast<float>(x),
                              .y = static_cast<float>(y),
                              .width = static_cast<float>(width),
                              .height = static_cast<float>(height),
                              .margin = static_cast<float>(margin),
                              .padding = static_cast<float>(padding)};
            const auto read_ids = [&](const Json::Value& ids, std::vector<std::string>& output,
                                      bool require_rule) -> std::optional<Error> {
                if (ids.size() > max_rule_nodes) {
                    return resource_error(ErrorCode::SecurityLimit, "layout group exceeds the node limit", group.id);
                }
                for (Json::ArrayIndex node_index = 0; node_index < ids.size(); ++node_index) {
                    if (!ids[node_index].isString()) {
                        return resource_error(ErrorCode::Validation, "layout group node must be a string", group.id);
                    }
                    const auto id = ids[node_index].asString();
                    if (id.empty() || id.size() > max_string_bytes || !valid_utf8(id)) {
                        return resource_error(ErrorCode::Validation, "invalid layout group node", group.id);
                    }
                    if (require_rule && !compiled.graph.index_by_id.contains(id)) {
                        if (result.warnings.size() < max_diagnostics) {
                            result.warnings.push_back(
                                resource_error(ErrorCode::Validation, "layout references an unknown rule ID", id));
                        }
                        continue;
                    }
                    output.push_back(id);
                }
                return std::nullopt;
            };
            const bool exact_ids = source == LayoutSource::Nodes || source == LayoutSource::Children;
            if (auto failure = read_ids(value["nodes"], group.nodes, exact_ids))
                return failure;
            if (auto failure = read_ids(value["exclude"], group.exclude, true))
                return failure;
            groups.push_back(std::move(group));
            if (value.isMember("children")) {
                if (!value["children"].isArray())
                    return resource_error(ErrorCode::Validation, "invalid nested layout", id);
                if (auto failure = self(self, value["children"], groups, x, y, width, height, true))
                    return failure;
            }
        }
        return std::nullopt;
    };
    if (auto failure = compile_groups(compile_groups, (*root)["main"], result.main, 0, 0, 1, 1, false)) {
        return ylt::unexpected<Error>{std::move(*failure)};
    }
    if (auto failure = compile_groups(compile_groups, (*root)["overlay"], result.overlay, 0, 0, 1, 1, false)) {
        return ylt::unexpected<Error>{std::move(*failure)};
    }
    normalize_layout_groups(result.main);
    normalize_layout_groups(result.overlay);
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
    const auto vendor_root = std::filesystem::weakly_canonical(asset_root / "assets/vender", ec);
    if (ec) {
        return ylt::unexpected<Error>{resource_error(ErrorCode::Io, "cannot resolve icon root", "assets/vender")};
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
        const auto original = presentation->icon_path;
        if (!original.empty()) {
            const auto invalid_icon = [&] {
                return ylt::unexpected<Error>{resource_error(
                    ErrorCode::Validation, "icon must resolve to a file inside its resource root", original)};
            };
            if (!valid_icon_reference(original)) {
                return ylt::unexpected<Error>{
                    resource_error(ErrorCode::Validation, "icon must be a normalized relative path", original)};
            }

            std::optional<std::filesystem::path> icon;
            std::filesystem::path namespaced_path;
            bool namespaced = false;
            const auto local = canonical_path(root, path_from_utf8(original));
            if (!local) {
                return invalid_icon();
            }
            if (regular_file(*local)) {
                icon = std::move(*local);
            }

            const auto slash = original.find('/');
            if (!icon && slash != std::string_view::npos) {
                const auto namespace_name = original.substr(0, slash);
                ec.clear();
                namespaced = namespace_name == "vender" ||
                             (std::filesystem::is_directory(vendor_root / namespace_name, ec) && !ec);
                if (namespaced) {
                    namespaced_path = namespace_name == "vender" ? path_from_utf8(original.substr(slash + 1))
                                                                 : path_from_utf8(original);
                }
            }

            if (!icon && namespaced) {
                const auto candidate = canonical_path(vendor_root, namespaced_path);
                if (!candidate) {
                    return invalid_icon();
                }
                if (regular_file(*candidate)) {
                    icon = std::move(*candidate);
                } else if (namespaced_path.extension().empty()) {
                    auto png_path = namespaced_path;
                    png_path += ".png";
                    const auto png = canonical_path(vendor_root, png_path);
                    if (!png) {
                        return invalid_icon();
                    }
                    if (regular_file(*png)) {
                        icon = std::move(*png);
                    }
                }
            }

            if (!icon) {
                if (!namespaced) {
                    return invalid_icon();
                }
                const auto fallback = canonical_path(vendor_root, "minecraft/missingno.xpm");
                if (!fallback || !regular_file(*fallback)) {
                    return ylt::unexpected<Error>{
                        resource_error(ErrorCode::Io, "missingno fallback icon is unavailable", "minecraft/missingno")};
                }
                icon = std::move(*fallback);
            }
            presentation->icon_path = path_to_utf8(*icon);
        }

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
            const auto frame = std::filesystem::weakly_canonical(asset_root / "assets/ui/widget" / filename, ec);
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

ylt::expected<IconFiles, Error> icon_file_stamps(const CompiledTemplate& compiled) {
    std::vector<std::filesystem::path> paths;
    for (const auto& view : compiled.presentation_by_node) {
        if (!view)
            continue;
        for (const auto* path : {&view->icon_path, &view->frame_obtained_path, &view->frame_unobtained_path}) {
            if (!path->empty())
                paths.emplace_back(path_from_utf8(*path));
        }
        if (!view->frame_obtained_path.empty())
            paths.push_back(path_from_utf8(view->frame_obtained_path).parent_path() / "frame_glow.png");
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
