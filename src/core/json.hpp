#pragma once

#include <beacon/core/model.hpp>

#include <json/json.h>

#include <initializer_list>
#include <string>
#include <string_view>

namespace beacon {

ylt::expected<Json::Value, Error> parse_json(std::string_view json, std::string_view context,
                                             std::string_view invalid_message = "invalid JSON");
bool exact_json_fields(const Json::Value& object, std::initializer_list<std::string_view> fields);

}  // namespace beacon
