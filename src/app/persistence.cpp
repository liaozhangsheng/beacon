#include <beacon/app/persistence.hpp>
#include <beacon/io/file.hpp>
#include "../core/json.hpp"

#include <json/json.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <string_view>

#if defined(_WIN32)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#else
    #include <fcntl.h>
    #include <sys/stat.h>
    #include <unistd.h>
#endif

namespace beacon {
namespace {

using JsonValidator = bool (*)(const Json::Value&);

Error path_error(ErrorCode code, std::string message, const std::filesystem::path& path) {
    return {.code = code, .message = std::move(message), .context = path_to_utf8(path.filename())};
}

bool sync_file(const std::filesystem::path& path) {
#if defined(_WIN32)
    const auto handle = CreateFileW(path.wstring().c_str(), GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    const auto ok = FlushFileBuffers(handle);
    CloseHandle(handle);
    return ok != 0;
#else
    const auto descriptor = open(path.c_str(), O_RDONLY);
    if (descriptor < 0) {
        return false;
    }
    const auto ok = fsync(descriptor) == 0;
    close(descriptor);
    return ok;
#endif
}

bool sync_directory(const std::filesystem::path& path) {
#if defined(_WIN32)
    (void)path;
    return true;
#else
    int flags = O_RDONLY;
    #if defined(O_DIRECTORY)
    flags |= O_DIRECTORY;
    #elif defined(O_NONBLOCK)
    flags |= O_NONBLOCK;
    #endif
    const auto descriptor = open(path.c_str(), flags);
    if (descriptor < 0) {
        return false;
    }
    struct stat info = {};
    const auto ok = fstat(descriptor, &info) == 0 && S_ISDIR(info.st_mode) && fsync(descriptor) == 0;
    close(descriptor);
    return ok;
#endif
}

bool replace_path(const std::filesystem::path& source, const std::filesystem::path& target, std::error_code& error) {
#if defined(_WIN32)
    if (!MoveFileExW(source.wstring().c_str(), target.wstring().c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
        return false;
    }
    error.clear();
    return true;
#else
    std::filesystem::rename(source, target, error);
    return !error;
#endif
}

ylt::expected<Json::Value, Error> read_json(const std::filesystem::path& path) {
    auto data = read_bounded_file(path, "state file");
    if (!data) {
        return ylt::unexpected<Error>{std::move(data.error())};
    }
    return parse_json(*data, path_to_utf8(path.filename()), "invalid state JSON");
}

ylt::expected<void, Error> replace_file_atomically(const std::filesystem::path& target, const std::string& json,
                                                   JsonValidator valid) {
    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
    if (ec) {
        return ylt::unexpected<Error>{path_error(ErrorCode::Io, "cannot create state directory", target)};
    }
    auto temporary = target;
    temporary += std::filesystem::path(".tmp-" + make_storage_key());
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(json.data(), static_cast<std::streamsize>(json.size()));
        output.flush();
        if (!output) {
            std::filesystem::remove(temporary, ec);
            return ylt::unexpected<Error>{path_error(ErrorCode::Io, "cannot write temporary state file", target)};
        }
    }
    if (!sync_file(temporary)) {
        std::filesystem::remove(temporary, ec);
        return ylt::unexpected<Error>{path_error(ErrorCode::Io, "cannot flush temporary state file", target)};
    }
    const auto verified = read_json(temporary);
    if (!verified || !valid(*verified)) {
        std::filesystem::remove(temporary, ec);
        return ylt::unexpected<Error>{path_error(ErrorCode::Internal, "temporary state verification failed", target)};
    }

    auto backup = target;
    backup += std::filesystem::path(".bak");
    const auto target_exists = std::filesystem::exists(target, ec);
    if (ec) {
        std::filesystem::remove(temporary, ec);
        return ylt::unexpected<Error>{path_error(ErrorCode::Io, "cannot inspect current state file", target)};
    }
    if (target_exists) {
        const auto current = read_json(target);
        if (current && valid(*current)) {
            std::filesystem::copy_file(target, backup, std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) {
                std::filesystem::remove(temporary, ec);
                return ylt::unexpected<Error>{path_error(ErrorCode::Io, "cannot preserve state backup", target)};
            }
        }
    }

    auto rollback = target;
    rollback += std::filesystem::path(".rollback-" + make_storage_key());
    if (target_exists) {
        std::filesystem::copy_file(target, rollback, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec || !sync_file(rollback)) {
            std::filesystem::remove(temporary, ec);
            std::filesystem::remove(rollback, ec);
            return ylt::unexpected<Error>{path_error(ErrorCode::Io, "cannot preserve rollback state", target)};
        }
    }
    if (!replace_path(temporary, target, ec)) {
        std::filesystem::remove(temporary, ec);
        std::filesystem::remove(rollback, ec);
        return ylt::unexpected<Error>{path_error(ErrorCode::Io, "cannot replace state file", target)};
    }
    if (!sync_directory(target.parent_path())) {
        bool restored = false;
        if (target_exists) {
            restored = replace_path(rollback, target, ec) && sync_directory(target.parent_path());
        } else {
            restored = std::filesystem::remove(target, ec) && !ec && sync_directory(target.parent_path());
        }
        if (!restored) {
            return ylt::unexpected<Error>{
                path_error(ErrorCode::Internal, "state commit failed with uncertain rollback", target)};
        }
        return ylt::unexpected<Error>{path_error(ErrorCode::Io, "state commit was rolled back", target)};
    }
    std::filesystem::remove(rollback, ec);
    return {};
}

std::string write_json(const Json::Value& value) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "  ";
    return Json::writeString(builder, value);
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

std::string make_storage_key() {
    std::array<std::uint32_t, 4> words{};
    std::random_device random;
    for (auto& word : words) {
        word = random();
    }
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto word : words) {
        output << std::setw(8) << word;
    }
    return output.str();
}

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
    return replace_file_atomically(path, write_json(root), valid_settings_json);
}

}  // namespace beacon
