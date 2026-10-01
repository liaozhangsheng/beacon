#include <beacon/app/persistence.hpp>
#include <beacon/io/file.hpp>
#include "../core/json.hpp"

#include <json/json.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace beacon {
namespace {

using JsonValidator = bool (*)(const Json::Value&);

Error path_error(ErrorCode code, std::string message, const std::filesystem::path& path) {
    return {.code = code, .message = std::move(message), .context = path_to_utf8(path.filename())};
}

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
        return ylt::unexpected<Error>{path_error(ErrorCode::Io, "cannot create state directory", target)};
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
            return ylt::unexpected<Error>{path_error(ErrorCode::Io, "cannot preserve state backup", target)};
        }
    }
    return write_file_atomically(target, json);
}

bool short_string(const Json::Value& value) {
    if (!value.isString())
        return false;
    const auto text = value.asString();
    return text.size() <= max_string_bytes && text.find('\0') == std::string::npos && valid_utf8(text);
}

bool valid_settings_json(const Json::Value& root) {
    if (!root.isObject())
        return false;
    auto fields = root;
    fields.removeMember("auto_detect");
    if ((root.isMember("auto_detect") && !root["auto_detect"].isBool()) ||
        !exact_json_fields(fields, {"game_root", "template_path", "language", "main_window_scale",
                                    "main_window_background_color", "overlay_visible", "overlay_transparent",
                                    "overlay_scroll_right", "overlay_window_scale", "overlay_window_background_color",
                                    "overlay_scroll_speed"}) ||
        !short_string(root["game_root"]) || root["game_root"].asString().empty() ||
        !short_string(root["template_path"]) || root["template_path"].asString().empty() ||
        !short_string(root["language"]) ||
        (!root["language"].asString().empty() && !valid_language(root["language"].asString())) ||
        !root["overlay_visible"].isBool() || !root["overlay_transparent"].isBool() ||
        !root["overlay_scroll_right"].isBool()) {
        return false;
    }
    const auto valid_scroll_speed = [](const Json::Value& value) {
        return value.isNumeric() && std::isfinite(value.asDouble()) && value.asDouble() >= 0.0 &&
               value.asDouble() <= 300.0;
    };
    const auto valid_color = [](const Json::Value& value) {
        if (!value.isArray() || value.size() != 3) {
            return false;
        }
        return std::all_of(value.begin(), value.end(), [](const Json::Value& component) {
            return component.isNumeric() && std::isfinite(component.asDouble()) && component.asDouble() >= 0.0 &&
                   component.asDouble() <= 1.0;
        });
    };
    const auto valid_scale = [](const Json::Value& value) {
        return value.isNumeric() && std::isfinite(value.asDouble()) && value.asDouble() >= min_window_scale &&
               value.asDouble() <= max_window_scale;
    };
    return valid_scale(root["main_window_scale"]) && valid_color(root["main_window_background_color"]) &&
           valid_scale(root["overlay_window_scale"]) && valid_color(root["overlay_window_background_color"]) &&
           valid_scroll_speed(root["overlay_scroll_speed"]);
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
    return ylt::unexpected<Error>{path_error(ErrorCode::Validation, std::string(invalid_message), path)};
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
        return ylt::unexpected<Error>{path_error(ErrorCode::Io, "cannot inspect state file", path)};
    }
    ec.clear();
    const auto backup_exists = std::filesystem::exists(backup, ec);
    if (ec) {
        return ylt::unexpected<Error>{path_error(ErrorCode::Io, "cannot inspect state backup", path)};
    }
    if (!primary_exists && !backup_exists) {
        return std::optional<Settings>{};
    }
    auto root = read_validated(path, valid_settings_json,
                               "settings.json and backup are invalid; expected game_root and template_path");
    if (!root) {
        return ylt::unexpected<Error>{std::move(root.error())};
    }
    Settings settings;
    settings.game_root = path_from_utf8((*root)["game_root"].asString());
    settings.template_path = path_from_utf8((*root)["template_path"].asString());
    settings.auto_detect = (*root).get("auto_detect", true).asBool();
    settings.language = (*root)["language"].asString();
    settings.main_window_scale = (*root)["main_window_scale"].asFloat();
    for (Json::ArrayIndex index = 0; index < 3; ++index) {
        settings.main_window_background_color[index] = (*root)["main_window_background_color"][index].asFloat();
    }
    settings.overlay_visible = (*root)["overlay_visible"].asBool();
    settings.overlay_transparent = (*root)["overlay_transparent"].asBool();
    settings.overlay_scroll_right = (*root)["overlay_scroll_right"].asBool();
    settings.overlay_window_scale = (*root)["overlay_window_scale"].asFloat();
    for (Json::ArrayIndex index = 0; index < 3; ++index) {
        settings.overlay_window_background_color[index] = (*root)["overlay_window_background_color"][index].asFloat();
    }
    settings.overlay_scroll_speed = (*root)["overlay_scroll_speed"].asFloat();
    return std::optional<Settings>{std::move(settings)};
}

ylt::expected<void, Error> Persistence::save_settings(const Settings& settings) const {
    const auto path = root_ / "config/settings.json";
    Json::Value root(Json::objectValue);
    root["game_root"] = path_to_utf8(settings.game_root);
    root["template_path"] = path_to_utf8(settings.template_path);
    root["auto_detect"] = settings.auto_detect;
    root["language"] = settings.language;
    root["main_window_scale"] = settings.main_window_scale;
    root["main_window_background_color"] = Json::arrayValue;
    for (const auto component : settings.main_window_background_color) {
        root["main_window_background_color"].append(component);
    }
    root["overlay_visible"] = settings.overlay_visible;
    root["overlay_transparent"] = settings.overlay_transparent;
    root["overlay_scroll_right"] = settings.overlay_scroll_right;
    root["overlay_window_scale"] = settings.overlay_window_scale;
    root["overlay_window_background_color"] = Json::arrayValue;
    for (const auto component : settings.overlay_window_background_color) {
        root["overlay_window_background_color"].append(component);
    }
    root["overlay_scroll_speed"] = settings.overlay_scroll_speed;
    if (!valid_settings_json(root)) {
        return ylt::unexpected<Error>{
            {.code = ErrorCode::Validation, .message = "invalid settings", .context = "settings"}};
    }
    return save_json(path, write_json(root), valid_settings_json);
}

}  // namespace beacon
