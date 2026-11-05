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
    Json::CharReaderBuilder builder;
    Json::CharReaderBuilder::strictMode(&builder.settings_);
    builder["collectComments"] = false;
    builder["rejectDupKeys"] = true;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    Json::Value root;
    bool parsed = false;
    try {  // jsoncpp throws once nesting exceeds its stack limit.
        parsed = reader->parse(json.data(), json.data() + json.size(), &root, nullptr);
    } catch (const std::exception&) {
    }
    if (!parsed)
        return ylt::unexpected<Error>{
            {.code = ErrorCode::Parse, .message = std::string(invalid_message), .context = std::string(context)}};
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
