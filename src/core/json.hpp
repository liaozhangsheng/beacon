#pragma once

#include <beacon/core/model.hpp>

#include <json/json.h>

#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

namespace beacon {

ylt::expected<Json::Value, Error> parse_json(std::string_view json, std::string_view context,
                                             std::string_view invalid_message = "invalid JSON");
bool exact_json_fields(const Json::Value& object, std::initializer_list<std::string_view> fields);
// Appends one JSON Pointer token to path.
std::string child_path(const std::string& path, std::string_view child);
std::optional<Error> reject_unknown_fields(const Json::Value& object, const std::string& path,
                                           std::initializer_list<std::string_view> allowed);

}  // namespace beacon
