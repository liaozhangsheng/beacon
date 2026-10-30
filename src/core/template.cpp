#include <beacon/core/model.hpp>
#include "json.hpp"

#include <json/json.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace beacon {
namespace {

constexpr auto no_index = std::numeric_limits<std::uint32_t>::max();

Error error(ErrorCode code, std::string message, std::string context) {
    return {.code = code, .message = std::move(message), .context = std::move(context)};
}

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

std::string child_path(const std::string& path, std::string_view child) {
    return path + '/' + pointer_token(child);
}

}  // namespace

bool valid_utf8(std::string_view text) {
    for (std::size_t index = 0; index < text.size();) {
        const auto first = static_cast<unsigned char>(text[index]);
        std::size_t length = 0;
        std::uint32_t codepoint = 0;
        if (first <= 0x7F) {
            length = 1;
            codepoint = first;
        } else if (first >= 0xC2 && first <= 0xDF) {
            length = 2;
            codepoint = first & 0x1F;
        } else if (first >= 0xE0 && first <= 0xEF) {
            length = 3;
            codepoint = first & 0x0F;
        } else if (first >= 0xF0 && first <= 0xF4) {
            length = 4;
            codepoint = first & 0x07;
        } else {
            return false;
        }
        if (index + length > text.size()) {
            return false;
        }
        for (std::size_t offset = 1; offset < length; ++offset) {
            const auto byte = static_cast<unsigned char>(text[index + offset]);
            if ((byte & 0xC0) != 0x80) {
                return false;
            }
            codepoint = (codepoint << 6) | (byte & 0x3F);
        }
        if ((length == 3 && codepoint < 0x800) || (length == 4 && codepoint < 0x10000) ||
            (codepoint >= 0xD800 && codepoint <= 0xDFFF) || codepoint > 0x10FFFF) {
            return false;
        }
        index += length;
    }
    return true;
}

bool valid_language(const std::string_view value) {
    return !value.empty() && value.size() <= 64 && std::all_of(value.begin(), value.end(), [](const char character) {
        return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '_' || character == '-';
    });
}

bool valid_path_component(const std::string_view value) {
    return !value.empty() && value.size() <= 128 && std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') ||
               byte == '_' || byte == '-';
    });
}

bool valid_icon_reference(std::string_view value) {
    if (value.find('\0') != std::string_view::npos || value.empty() || value.front() == '/' || value.front() == '\\' ||
        value.find('\\') != std::string_view::npos || value.find(':') != std::string_view::npos) {
        return false;
    }
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto end = value.find('/', start);
        const auto segment = value.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        if (segment.empty() || segment == "." || segment == "..") {
            return false;
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return true;
}

namespace {

std::optional<Error> check_string(const Json::Value& value, const std::string& path) {
    if (!value.isString()) {
        return error(ErrorCode::Validation, "expected a string", path);
    }
    const auto text = value.asString();
    if (text.size() > max_string_bytes) {
        return error(ErrorCode::SecurityLimit, "string exceeds the byte limit", path);
    }
    if (!valid_utf8(text)) {
        return error(ErrorCode::Validation, "string is not valid UTF-8", path);
    }
    return std::nullopt;
}

std::optional<Error> reject_unknown_fields(const Json::Value& object, const std::string& path,
                                           std::initializer_list<std::string_view> allowed) {
    for (const auto& name : object.getMemberNames()) {
        if (name.size() > max_string_bytes) {
            return error(ErrorCode::SecurityLimit, "field name exceeds the byte limit", child_path(path, name));
        }
        if (!valid_utf8(name)) {
            return error(ErrorCode::Validation, "field name is not valid UTF-8", path);
        }
        if (std::find(allowed.begin(), allowed.end(), name) == allowed.end()) {
            return error(ErrorCode::Validation, "unknown field '" + name + "'", child_path(path, name));
        }
    }
    return std::nullopt;
}

std::optional<Error> validate_rule_shape(const Json::Value& value, const std::string& path, std::size_t& node_count);

std::optional<Error> validate_children(const Json::Value& children, const std::string& path, std::size_t& node_count) {
    if (!children.isArray()) {
        return error(ErrorCode::Validation, "expected an array", path);
    }
    for (Json::ArrayIndex index = 0; index < children.size(); ++index) {
        if (auto failure = validate_rule_shape(children[index], child_path(path, std::to_string(index)), node_count)) {
            return failure;
        }
    }
    return std::nullopt;
}

std::optional<Error> validate_rule_shape(const Json::Value& value, const std::string& path, std::size_t& node_count) {
    if (!value.isObject()) {
        return error(ErrorCode::Validation, "expected a rule object", path);
    }
    if (++node_count > max_rule_nodes) {
        return error(ErrorCode::SecurityLimit, "template exceeds the rule node limit", path);
    }
    if (value.isMember("id")) {
        if (auto failure = check_string(value["id"], child_path(path, "id"))) {
            return failure;
        }
    }
    if (value.isMember("view")) {
        const auto view_path = child_path(path, "view");
        const auto& view = value["view"];
        if (!view.isObject()) {
            return error(ErrorCode::Validation, "expected an object", view_path);
        }
        if (auto failure = reject_unknown_fields(view, view_path, {"name_key", "icon", "type"})) {
            return failure;
        }
        if (!view.isMember("name_key")) {
            return error(ErrorCode::Validation, "missing required field 'name_key'", child_path(view_path, "name_key"));
        }
        if (auto failure = check_string(view["name_key"], child_path(view_path, "name_key"))) {
            return failure;
        }
        if (view.isMember("icon")) {
            if (auto failure = check_string(view["icon"], child_path(view_path, "icon"))) {
                return failure;
            }
        }
        if (view.isMember("type") &&
            (!view["type"].isString() || (view["type"].asString() != "task" && view["type"].asString() != "goal" &&
                                          view["type"].asString() != "challenge"))) {
            return error(ErrorCode::Validation, "type must be task, goal, or challenge", child_path(view_path, "type"));
        }
    }

    constexpr std::array operations{"fact", "ref", "all", "any", "count", "prefix"};
    std::string_view operation;
    std::size_t operation_count = 0;
    for (const auto candidate : operations) {
        if (value.isMember(candidate)) {
            operation = candidate;
            ++operation_count;
        }
    }
    if (operation_count > 1) {
        return error(ErrorCode::Validation, "rule must contain at most one operation", path);
    }
    if (operation_count == 0) {
        return reject_unknown_fields(value, path, {"id", "view"});
    }

    if (operation == "fact") {
        if (auto failure = reject_unknown_fields(value, path, {"id", "fact", "target", "view"})) {
            return failure;
        }
        if (auto failure = check_string(value["fact"], child_path(path, "fact"))) {
            return failure;
        }
        if (value.isMember("target") && value["target"].type() != Json::intValue) {
            return error(ErrorCode::Validation, "expected an int64 JSON integer", child_path(path, "target"));
        }
        if (value.isMember("target") && value["target"].asInt64() < 1) {
            return error(ErrorCode::Validation, "Fact target must be at least 1", child_path(path, "target"));
        }
        return std::nullopt;
    }
    if (operation == "ref") {
        if (auto failure = reject_unknown_fields(value, path, {"id", "ref", "view"})) {
            return failure;
        }
        return check_string(value["ref"], child_path(path, "ref"));
    }
    if (operation == "count") {
        if (auto failure = reject_unknown_fields(value, path, {"id", "count", "view"})) {
            return failure;
        }
        const auto& count = value["count"];
        if (!count.isObject()) {
            return error(ErrorCode::Validation, "expected an object", child_path(path, "count"));
        }
        const auto count_path = child_path(path, "count");
        if (auto failure = reject_unknown_fields(count, count_path, {"target", "of"})) {
            return failure;
        }
        if (!count.isMember("target") || count["target"].type() != Json::intValue) {
            return error(ErrorCode::Validation, "expected an int64 JSON integer", child_path(count_path, "target"));
        }
        if (!count.isMember("of")) {
            return error(ErrorCode::Validation, "missing required field 'of'", child_path(count_path, "of"));
        }
        return validate_children(count["of"], child_path(count_path, "of"), node_count);
    }

    if (auto failure = reject_unknown_fields(value, path, {"id", operation, "view"})) {
        return failure;
    }
    return validate_children(value[std::string(operation)], child_path(path, operation), node_count);
}

std::optional<Error> validate_document_shape(const Json::Value& root) {
    if (!root.isObject()) {
        return error(ErrorCode::Validation, "template root must be an object", "");
    }
    if (auto failure = reject_unknown_fields(root, "", {"name_key", "minecraft", "goals", "completion_rule"})) {
        return failure;
    }
    if (!root.isMember("goals") || !root["goals"].isArray()) {
        return error(ErrorCode::Validation, "expected an array", "/goals");
    }
    if (!root.isMember("name_key")) {
        return error(ErrorCode::Validation, "missing required field 'name_key'", "/name_key");
    }
    if (auto failure = check_string(root["name_key"], "/name_key")) {
        return failure;
    }
    if (!root.isMember("minecraft") || !root["minecraft"].isObject()) {
        return error(ErrorCode::Validation, "expected an object", "/minecraft");
    }
    const auto& minecraft = root["minecraft"];
    if (auto failure = reject_unknown_fields(minecraft, "/minecraft", {"min_version", "max_version"})) {
        return failure;
    }
    for (const auto field : {"min_version", "max_version"}) {
        if (!minecraft.isMember(field) || minecraft[field].type() != Json::intValue || minecraft[field].asInt64() < 0) {
            return error(ErrorCode::Validation, std::string(field) + " must be a non-negative integer",
                         child_path("/minecraft", field));
        }
    }
    if (minecraft["max_version"].asInt64() < minecraft["min_version"].asInt64()) {
        return error(ErrorCode::Validation, "minecraft min_version must not exceed max_version", "/minecraft");
    }
    if (!root.isMember("completion_rule")) {
        return error(ErrorCode::Validation, "missing required field 'completion_rule'", "/completion_rule");
    }
    if (auto failure = check_string(root["completion_rule"], "/completion_rule")) {
        return failure;
    }

    std::size_t node_count = 0;
    for (Json::ArrayIndex index = 0; index < root["goals"].size(); ++index) {
        if (auto failure = validate_rule_shape(root["goals"][index], "/goals/" + std::to_string(index), node_count)) {
            return failure;
        }
    }
    return std::nullopt;
}

class Diagnostics {
public:
    void add(std::string message, std::string context) {
        if (errors_.size() < max_diagnostics) {
            errors_.push_back(error(ErrorCode::Validation, std::move(message), std::move(context)));
        } else if (!truncated_) {
            errors_.back() = error(ErrorCode::Validation, "additional diagnostics were truncated", "");
            truncated_ = true;
        }
    }

    [[nodiscard]] bool empty() const {
        return errors_.empty();
    }
    TemplateErrors take() {
        return std::move(errors_);
    }

private:
    TemplateErrors errors_;
    bool truncated_ = false;
};

struct Compiler {
    CompiledTemplate compiled;
    Diagnostics diagnostics;
    std::vector<std::string> paths;
    std::vector<std::string> ref_names;

    std::uint32_t add_rule(const Json::Value& value, const std::string& path) {
        const auto index = static_cast<std::uint32_t>(compiled.graph.nodes.size());
        compiled.graph.nodes.emplace_back();
        compiled.presentation_by_node.emplace_back();
        paths.push_back(path);
        ref_names.emplace_back();

        auto& node = compiled.graph.nodes[index];
        if (value.isMember("id")) {
            node.id = value["id"].asString();
            if (node.id.empty()) {
                diagnostics.add("explicit rule ID must not be empty", child_path(path, "id"));
            } else if (!compiled.graph.index_by_id.emplace(node.id, index).second) {
                diagnostics.add("duplicate rule ID '" + node.id + "'", child_path(path, "id"));
            }
        }

        if (value.isMember("view")) {
            const auto& view = value["view"];
            const auto view_path = child_path(path, "view");
            Presentation presentation;
            presentation.localization_key = view["name_key"].asString();
            presentation.icon_path = view.isMember("icon") ? view["icon"].asString() : std::string{};
            if (view.isMember("type")) {
                const auto type = view["type"].asString();
                presentation.frame_type = type == "goal"        ? Presentation::FrameType::Goal
                                          : type == "challenge" ? Presentation::FrameType::Challenge
                                                                : Presentation::FrameType::Task;
            }
            if (presentation.localization_key.empty()) {
                diagnostics.add("view name_key must not be empty", child_path(view_path, "name_key"));
            }
            if (!presentation.icon_path.empty() && !valid_icon_reference(presentation.icon_path)) {
                diagnostics.add("icon must be a normalized relative path", child_path(view_path, "icon"));
            }
            compiled.presentation_by_node[index] = std::move(presentation);
        }

        if (value.isMember("fact")) {
            node.op = RuleOp::Fact;
            node.fact_key = value["fact"].asString();
            node.target =
                value.isMember("target") ? value["target"].asInt64() : (node.fact_key.starts_with("stat/") ? 0 : 1);
            if (node.fact_key.empty()) {
                diagnostics.add("Fact key must not be empty", child_path(path, "fact"));
            }
        } else if (value.isMember("ref")) {
            node.op = RuleOp::Ref;
            ref_names[index] = value["ref"].asString();
            if (ref_names[index].empty()) {
                diagnostics.add("Ref target must not be empty", child_path(path, "ref"));
            }
        } else if (!value.isMember("all") && !value.isMember("any") && !value.isMember("count") &&
                   !value.isMember("prefix")) {
            node.op = RuleOp::Fact;
            node.fact_key = "manual/" + (node.id.empty() ? path : node.id);
            node.target = 1;
        } else {
            const Json::Value* children = nullptr;
            std::string children_path;
            if (value.isMember("all")) {
                compiled.graph.nodes[index].op = RuleOp::All;
                children = &value["all"];
                children_path = child_path(path, "all");
            } else if (value.isMember("any")) {
                compiled.graph.nodes[index].op = RuleOp::Any;
                children = &value["any"];
                children_path = child_path(path, "any");
            } else if (value.isMember("prefix")) {
                compiled.graph.nodes[index].op = RuleOp::Prefix;
                children = &value["prefix"];
                children_path = child_path(path, "prefix");
            } else {
                compiled.graph.nodes[index].op = RuleOp::Count;
                compiled.graph.nodes[index].target = value["count"]["target"].asInt64();
                children = &value["count"]["of"];
                children_path = child_path(child_path(path, "count"), "of");
            }

            if (children->empty()) {
                diagnostics.add("aggregate rule must contain at least one child", children_path);
            }
            for (Json::ArrayIndex child = 0; child < children->size(); ++child) {
                const auto child_index = add_rule((*children)[child], child_path(children_path, std::to_string(child)));
                compiled.graph.nodes[index].children.push_back(child_index);
            }
            const auto& completed_node = compiled.graph.nodes[index];
            if (completed_node.op == RuleOp::Count &&
                (completed_node.target < 1 ||
                 completed_node.target > static_cast<std::int64_t>(completed_node.children.size()))) {
                diagnostics.add("Count target must be between 1 and its child count",
                                child_path(child_path(path, "count"), "target"));
            }
        }
        return index;
    }

    void resolve_refs() {
        for (std::uint32_t index = 0; index < ref_names.size(); ++index) {
            if (ref_names[index].empty()) {
                continue;
            }
            const auto target = compiled.graph.index_by_id.find(ref_names[index]);
            if (target == compiled.graph.index_by_id.end()) {
                diagnostics.add("missing Ref target '" + ref_names[index] + "'", child_path(paths[index], "ref"));
            } else {
                compiled.graph.nodes[index].reference_node = target->second;
            }
        }
    }

    std::string label(std::uint32_t index) const {
        return compiled.graph.nodes[index].id.empty() ? paths[index] : compiled.graph.nodes[index].id;
    }

    void build_evaluation_order() {
        struct Frame {
            std::uint32_t index;
            std::size_t next_dependency = 0;
        };

        std::vector<std::uint8_t> colors(compiled.graph.nodes.size());
        std::vector<Frame> stack;
        stack.reserve(compiled.graph.nodes.size());

        for (std::uint32_t root = 0; root < compiled.graph.nodes.size(); ++root) {
            if (colors[root] != 0) {
                continue;
            }
            colors[root] = 1;
            stack.push_back({root});

            while (!stack.empty()) {
                auto& frame = stack.back();
                const auto& node = compiled.graph.nodes[frame.index];
                const auto has_ref = node.op == RuleOp::Ref && node.reference_node != no_index;
                const auto dependency_count = node.children.size() + (has_ref ? 1U : 0U);

                if (frame.next_dependency == dependency_count) {
                    colors[frame.index] = 2;
                    compiled.graph.evaluation_order.push_back(frame.index);
                    stack.pop_back();
                    continue;
                }

                const auto dependency = frame.next_dependency < node.children.size()
                                            ? node.children[frame.next_dependency]
                                            : node.reference_node;
                ++frame.next_dependency;
                if (colors[dependency] == 0) {
                    colors[dependency] = 1;
                    stack.push_back({dependency});
                } else if (colors[dependency] == 1) {
                    const auto cycle_start = std::find_if(stack.begin(), stack.end(), [&](const Frame& candidate) {
                        return candidate.index == dependency;
                    });
                    std::ostringstream message;
                    message << "dependency cycle: ";
                    for (auto current = cycle_start; current != stack.end(); ++current) {
                        if (current != cycle_start) {
                            message << " -> ";
                        }
                        message << label(current->index);
                    }
                    message << " -> " << label(dependency);
                    diagnostics.add(message.str(), paths[dependency]);
                }
            }
        }
    }
};

}  // namespace

ylt::expected<CompiledTemplate, TemplateErrors> compile_template_json(std::string_view json) {
    auto parsed_json = beacon::parse_json(json, "template.json", "invalid template JSON");
    if (!parsed_json) {
        return ylt::unexpected<TemplateErrors>{{std::move(parsed_json.error())}};
    }
    auto parsed = std::move(*parsed_json);
    if (auto failure = validate_document_shape(parsed)) {
        return ylt::unexpected<TemplateErrors>{{std::move(*failure)}};
    }

    Compiler compiler;
    if (parsed["goals"].empty()) {
        compiler.diagnostics.add("goals must contain at least one rule", "/goals");
    }
    compiler.compiled.template_name_key = parsed["name_key"].asString();
    if (compiler.compiled.template_name_key.empty()) {
        compiler.diagnostics.add("template name_key must not be empty", "/name_key");
    }

    compiler.compiled.minecraft.min_data_version = parsed["minecraft"]["min_version"].asInt64();
    compiler.compiled.minecraft.max_data_version = parsed["minecraft"]["max_version"].asInt64();

    for (Json::ArrayIndex index = 0; index < parsed["goals"].size(); ++index) {
        compiler.add_rule(parsed["goals"][index], "/goals/" + std::to_string(index));
    }

    compiler.resolve_refs();
    const auto completion_name = parsed["completion_rule"].asString();
    if (completion_name.empty()) {
        compiler.diagnostics.add("completion_rule must not be empty", "/completion_rule");
    } else if (const auto completion = compiler.compiled.graph.index_by_id.find(completion_name);
               completion != compiler.compiled.graph.index_by_id.end()) {
        compiler.compiled.completion_node = completion->second;
    } else {
        compiler.diagnostics.add("completion_rule does not name an explicit rule ID", "/completion_rule");
    }

    compiler.build_evaluation_order();
    if (!compiler.diagnostics.empty()) {
        return ylt::unexpected<TemplateErrors>{compiler.diagnostics.take()};
    }
    return std::move(compiler.compiled);
}

}  // namespace beacon
