#include <beacon/app/persistence.hpp>
#include <beacon/io/file.hpp>
#include "../core/json.hpp"

#include <json/json.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

namespace beacon {
namespace {

using JsonValidator = bool (*)(const Json::Value&);

ylt::expected<Json::Value, Error> read_json(const std::filesystem::path& path) {
    auto data = read_bounded_file(path, "state file");
    if (!data) {
        return ylt::unexpected<Error>{std::move(data.error())};
    }
    return parse_json(*data, path_to_utf8(path.filename()), "invalid state JSON");
}

std::string write_json(const Json::Value& value) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "  ";
    return Json::writeString(builder, value);
}

// The previous valid file stays in .bak, which read_validated falls back to
// when the primary file becomes invalid, for example after a hand edit.
ylt::expected<void, Error> save_json(const std::filesystem::path& target, const std::string& json,
                                     JsonValidator valid) {
    std::error_code error;
    std::filesystem::create_directories(target.parent_path(), error);
    if (error) {
        return ylt::unexpected<Error>{file_error(ErrorCode::Io, "cannot create state directory", target)};
    }
    // A recovered backup must remain selected if this directory cannot be
    // opened for flushing after replacing the missing primary file.
    if (auto synced = sync_path(target.parent_path(), true); !synced) {
        return synced;
    }
    auto backup = target;
    backup += std::filesystem::path(".bak");
    if (const auto current = read_json(target); current && valid(*current)) {
        if (auto saved = write_file_atomically(backup, write_json(*current)); !saved) {
            return ylt::unexpected<Error>{file_error(ErrorCode::Io, "cannot preserve state backup", target)};
        }
    }
    return write_file_atomically(target, json);
}

bool short_string(const Json::Value& value) {
    if (!value.isString())
        return false;
    return value.asString().find('\0') == std::string::npos;
}

bool valid_dimension(const Json::Value& value) {
    return (value.type() == Json::intValue || value.type() == Json::uintValue) && value.isInt() && value.asInt() >= 1 &&
           value.asInt() <= max_window_dimension;
}

bool valid_scale(const Json::Value& value) {
    return value.isNumeric() && std::isfinite(value.asDouble()) && value.asDouble() >= min_window_scale &&
           value.asDouble() <= max_window_scale;
}

bool valid_color(const Json::Value& value) {
    return value.isArray() && value.size() == 3 && std::all_of(value.begin(), value.end(), [](const Json::Value& part) {
               return part.isNumeric() && std::isfinite(part.asDouble()) && part.asDouble() >= 0.0 &&
                      part.asDouble() <= 1.0;
           });
}

bool valid_settings_json(const Json::Value& root) {
    if (!root.isObject() ||
        reject_unknown_fields(root, "settings.json",
                              {"ui_language", "auto_detect", "game_root", "template_path", "language",
                               "main_window_width", "main_window_height", "main_window_scale",
                               "main_window_background_color", "overlay_visible", "overlay_window_width",
                               "overlay_window_height", "overlay_window_scale", "overlay_transparent",
                               "overlay_window_background_color", "overlay_scroll_right", "overlay_scroll_speed"}))
        return false;
    const auto required_path = [&](const char* name) {
        return short_string(root[name]) && !root[name].asString().empty();
    };
    const auto optional = [&](const char* name, const auto valid) {
        return !root.isMember(name) || valid(root[name]);
    };
    const auto boolean = [](const Json::Value& value) {
        return value.isBool();
    };
    // An empty game root is saved as is; the runtime then reports the save directory as not configured.
    return optional("ui_language",
                    [](const Json::Value& value) {
                        return value.isString() && (value.asString() == "zh" || value.asString() == "en");
                    }) &&
           optional("auto_detect", boolean) && short_string(root["game_root"]) && required_path("template_path") &&
           optional("language",
                    [](const Json::Value& value) {
                        return short_string(value) && (value.asString().empty() || valid_language(value.asString()));
                    }) &&
           optional("main_window_width", valid_dimension) && optional("main_window_height", valid_dimension) &&
           optional("main_window_scale", valid_scale) && optional("main_window_background_color", valid_color) &&
           optional("overlay_visible", boolean) && optional("overlay_window_width", valid_dimension) &&
           optional("overlay_window_height", valid_dimension) && optional("overlay_window_scale", valid_scale) &&
           optional("overlay_transparent", boolean) && optional("overlay_window_background_color", valid_color) &&
           optional("overlay_scroll_right", boolean) && optional("overlay_scroll_speed", [](const Json::Value& value) {
               return value.isNumeric() && std::isfinite(value.asDouble()) && value.asDouble() >= 0.0 &&
                      value.asDouble() <= 300.0;
           });
}

ylt::expected<Json::Value, Error>
read_validated(const std::filesystem::path& path, JsonValidator valid,
               std::string_view invalid_message = "state file and backup are invalid") {
    auto primary = read_json(path);
    if (primary && valid(*primary)) {
        return primary;
    }
    auto backup_path = path;
    backup_path += std::filesystem::path(".bak");
    auto backup = read_json(backup_path);
    if (backup && valid(*backup)) {
        return backup;
    }
    return ylt::unexpected<Error>{file_error(ErrorCode::Validation, std::string(invalid_message), path)};
}

}  // namespace

Persistence::Persistence(std::filesystem::path root) : root_(std::move(root)) {}

ylt::expected<std::optional<Settings>, Error> Persistence::load_settings() const {
    const auto path = root_ / "config/settings.json";
    auto backup = path;
    backup += std::filesystem::path(".bak");
    std::error_code ec;
    const auto primary_exists = std::filesystem::exists(path, ec);
    if (ec) {
        return ylt::unexpected<Error>{file_error(ErrorCode::Io, "cannot inspect state file", path)};
    }
    ec.clear();
    const auto backup_exists = std::filesystem::exists(backup, ec);
    if (ec) {
        return ylt::unexpected<Error>{file_error(ErrorCode::Io, "cannot inspect state backup", path)};
    }
    if (!primary_exists && !backup_exists) {
        return std::optional<Settings>{};
    }
    auto root = read_validated(path, valid_settings_json,
                               "settings.json and backup are invalid; expected game_root and template_path");
    if (!root) {
        return ylt::unexpected<Error>{std::move(root.error())};
    }
    const auto& fields = *root;
    Settings settings;
    const auto read_color = [&](const char* name, std::array<float, 3>& color) {
        if (fields.isMember(name)) {
            for (Json::ArrayIndex index = 0; index < color.size(); ++index)
                color[index] = fields[name][index].asFloat();
        }
    };
    settings.ui_language = fields.get("ui_language", settings.ui_language).asString();

    settings.auto_detect = fields.get("auto_detect", settings.auto_detect).asBool();
    settings.game_root = path_from_utf8(fields["game_root"].asString());
    settings.template_path = path_from_utf8(fields["template_path"].asString());
    settings.language = fields.get("language", settings.language).asString();

    settings.main_window_width = fields.get("main_window_width", settings.main_window_width).asInt();
    settings.main_window_height = fields.get("main_window_height", settings.main_window_height).asInt();
    settings.main_window_scale = fields.get("main_window_scale", settings.main_window_scale).asFloat();
    read_color("main_window_background_color", settings.main_window_background_color);

    settings.overlay_visible = fields.get("overlay_visible", settings.overlay_visible).asBool();
    settings.overlay_window_width = fields.get("overlay_window_width", settings.overlay_window_width).asInt();
    settings.overlay_window_height = fields.get("overlay_window_height", settings.overlay_window_height).asInt();
    settings.overlay_window_scale = fields.get("overlay_window_scale", settings.overlay_window_scale).asFloat();
    settings.overlay_transparent = fields.get("overlay_transparent", settings.overlay_transparent).asBool();
    read_color("overlay_window_background_color", settings.overlay_window_background_color);
    settings.overlay_scroll_right = fields.get("overlay_scroll_right", settings.overlay_scroll_right).asBool();
    settings.overlay_scroll_speed = fields.get("overlay_scroll_speed", settings.overlay_scroll_speed).asFloat();
    return std::optional<Settings>{std::move(settings)};
}

ylt::expected<void, Error> Persistence::save_settings(const Settings& settings) const {
    const auto path = root_ / "config/settings.json";
    Json::Value root(Json::objectValue);
    root["ui_language"] = settings.ui_language;

    root["auto_detect"] = settings.auto_detect;
    root["game_root"] = path_to_utf8(settings.game_root);
    root["template_path"] = path_to_utf8(settings.template_path);
    root["language"] = settings.language;

    root["main_window_width"] = settings.main_window_width;
    root["main_window_height"] = settings.main_window_height;
    root["main_window_scale"] = settings.main_window_scale;
    root["main_window_background_color"] = Json::arrayValue;
    for (const auto component : settings.main_window_background_color)
        root["main_window_background_color"].append(component);

    root["overlay_visible"] = settings.overlay_visible;
    root["overlay_window_width"] = settings.overlay_window_width;
    root["overlay_window_height"] = settings.overlay_window_height;
    root["overlay_window_scale"] = settings.overlay_window_scale;
    root["overlay_transparent"] = settings.overlay_transparent;
    root["overlay_window_background_color"] = Json::arrayValue;
    for (const auto component : settings.overlay_window_background_color)
        root["overlay_window_background_color"].append(component);
    root["overlay_scroll_right"] = settings.overlay_scroll_right;
    root["overlay_scroll_speed"] = settings.overlay_scroll_speed;
    if (!valid_settings_json(root)) {
        return ylt::unexpected<Error>{
            {.code = ErrorCode::Validation, .message = "invalid settings", .context = "settings"}};
    }
    return save_json(path, write_json(root), valid_settings_json);
}

}  // namespace beacon
