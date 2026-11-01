#include "json.hpp"

#include <algorithm>
#include <exception>
#include <memory>

namespace beacon {
namespace {

std::string pointer_token(std::string_view token) {
    std::string escaped;
    for (const char character : token) {
        if (character == '~') {
            escaped += "~0";
        } else if (character == '/') {
            escaped += "~1";
        } else {
            escaped += character;
        }
    }
    return escaped;
}

}  // namespace

std::string child_path(const std::string& path, std::string_view child) {
    return path + '/' + pointer_token(child);
}

std::optional<Error> reject_unknown_fields(const Json::Value& object, const std::string& path,
                                           std::initializer_list<std::string_view> allowed) {
    for (const auto& name : object.getMemberNames()) {
        if (name.size() > max_string_bytes) {
            return Error{.code = ErrorCode::SecurityLimit,
                         .message = "field name exceeds the byte limit",
                         .context = child_path(path, name)};
        }
        if (!valid_utf8(name)) {
            return Error{.code = ErrorCode::Validation, .message = "field name is not valid UTF-8", .context = path};
        }
        if (std::find(allowed.begin(), allowed.end(), name) == allowed.end()) {
            return Error{.code = ErrorCode::Validation,
                         .message = "unknown field '" + name + "'",
                         .context = child_path(path, name)};
        }
    }
    return std::nullopt;
}

ylt::expected<Json::Value, Error> parse_json(std::string_view json, std::string_view context,
                                             std::string_view invalid_message) {
    if (json.size() > max_json_bytes) {
        return ylt::unexpected<Error>{
            {.code = ErrorCode::SecurityLimit, .message = "JSON exceeds byte limit", .context = std::string(context)}};
    }
    Json::CharReaderBuilder builder;
    Json::CharReaderBuilder::strictMode(&builder.settings_);
    builder["collectComments"] = false;
    builder["rejectDupKeys"] = true;
    builder["stackLimit"] = static_cast<Json::UInt>(max_json_depth + 1);
    Json::Value root;
    std::string parser_error;
    try {
        const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
        if (!reader) {
            return ylt::unexpected<Error>{
                {.code = ErrorCode::Internal, .message = "cannot create JSON reader", .context = std::string(context)}};
        }
        if (!reader->parse(json.data(), json.data() + json.size(), &root, &parser_error)) {
            return ylt::unexpected<Error>{
                {.code = ErrorCode::Parse, .message = std::string(invalid_message), .context = std::string(context)}};
        }
    } catch (const std::exception&) {
        return ylt::unexpected<Error>{{.code = ErrorCode::SecurityLimit,
                                       .message = "JSON nesting exceeds the limit",
                                       .context = std::string(context)}};
    }
    return root;
}

}  // namespace beacon

bool beacon::exact_json_fields(const Json::Value& object, std::initializer_list<std::string_view> fields) {
    if (!object.isObject() || object.size() != fields.size()) {
        return false;
    }
    return std::all_of(fields.begin(), fields.end(), [&](const auto field) {
        return object.isMember(std::string(field));
    });
}
