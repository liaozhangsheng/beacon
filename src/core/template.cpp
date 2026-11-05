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
#include <string>
#include <utility>
#include <vector>

namespace beacon {
namespace {

constexpr auto no_index = std::numeric_limits<std::uint32_t>::max();

}  // namespace

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

std::optional<Error> read_state_styles(const Json::Value& value, std::array<StateStyles, 2>& styles,
                                       const std::string& path) {
    const auto invalid = [](const std::string& where) {
        return Error{ErrorCode::Validation, "invalid state_style", where};
    };
    if (!value.isObject())
        return invalid(path);
    for (const auto& window : value.getMemberNames()) {
        const auto window_path = child_path(path, window);
        if ((window != "main" && window != "overlay") || !value[window].isObject())
            return invalid(window_path);
        const bool overlay = window == "overlay";
        for (const auto& state : value[window].getMemberNames()) {
            const auto state_path = child_path(window_path, state);
            const auto& fields = value[window][state];
            if ((state != "pending" && state != "done") || !fields.isObject())
                return invalid(state_path);
            auto& style = state == "done" ? styles[overlay].done : styles[overlay].pending;
            for (const auto& name : fields.getMemberNames()) {
                const auto field_path = child_path(state_path, name);
                if (!fields[name].isString())
                    return invalid(field_path);
                const auto setting = fields[name].asString();
                if (name == "emphasis") {
                    if (setting == "highlight")
                        style.emphasis = ItemEmphasis::Highlight;
                    else if (setting == "muted")
                        style.emphasis = ItemEmphasis::Muted;
                    else
                        return invalid(field_path);
                } else if (name == "visibility") {
                    if (setting == "visible")
                        style.visibility = ItemVisibility::Visible;
                    else if (setting == "blank")
                        style.visibility = ItemVisibility::Blank;
                    else if (setting == "hidden")
                        style.visibility = ItemVisibility::Hidden;
                    else
                        return invalid(field_path);
                } else if (name == "animation") {
                    if (!overlay || state != "done" || (setting != "none" && setting != "cover"))
                        return invalid(field_path);
                    style.animation = setting == "cover" ? ItemAnimation::Cover : ItemAnimation::None;
                } else if (name == "decoration") {
                    if (overlay || (setting != "none" && setting != "strikethrough"))
                        return invalid(field_path);
                    style.strikethrough = setting == "strikethrough";
                } else {
                    return invalid(field_path);
                }
            }
        }
    }
    return std::nullopt;
}

// jsoncpp stores integers above INT32_MAX as unsigned, so check the range rather than the type.
bool integer(const Json::Value& value) {
    return (value.type() == Json::intValue || value.type() == Json::uintValue) && value.isInt64();
}

std::optional<Error> check_string(const Json::Value& value, const std::string& path) {
    if (!value.isString())
        return Error{ErrorCode::Validation, "expected a string", path};
    return std::nullopt;
}

std::optional<Error> validate_rule_shape(const Json::Value& value, const std::string& path);

std::optional<Error> validate_children(const Json::Value& children, const std::string& path) {
    if (!children.isArray()) {
        return Error{ErrorCode::Validation, "expected an array", path};
    }
    for (Json::ArrayIndex index = 0; index < children.size(); ++index) {
        if (auto failure = validate_rule_shape(children[index], child_path(path, std::to_string(index)))) {
            return failure;
        }
    }
    return std::nullopt;
}

std::optional<Error> validate_rule_shape(const Json::Value& value, const std::string& path) {
    if (!value.isObject()) {
        return Error{ErrorCode::Validation, "expected a rule object", path};
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
            return Error{ErrorCode::Validation, "expected an object", view_path};
        }
        if (auto failure = reject_unknown_fields(view, view_path,
                                                 {"name_key", "icon", "enchanted", "type", "show", "state_style"})) {
            return failure;
        }
        if (!view.isMember("name_key")) {
            return Error{ErrorCode::Validation, "missing required field 'name_key'", child_path(view_path, "name_key")};
        }
        if (auto failure = check_string(view["name_key"], child_path(view_path, "name_key"))) {
            return failure;
        }
        if (view.isMember("icon")) {
            if (auto failure = check_string(view["icon"], child_path(view_path, "icon"))) {
                return failure;
            }
        }
        if (view.isMember("enchanted") && !view["enchanted"].isBool()) {
            return Error{ErrorCode::Validation, "expected a boolean", child_path(view_path, "enchanted")};
        }
        if (view.isMember("type") &&
            (!view["type"].isString() || (view["type"].asString() != "task" && view["type"].asString() != "goal" &&
                                          view["type"].asString() != "challenge"))) {
            return Error{ErrorCode::Validation, "type must be task, goal, or challenge", child_path(view_path, "type")};
        }
        if (view.isMember("show") &&
            (!view["show"].isString() || (view["show"].asString() != "self" && view["show"].asString() != "stages" &&
                                          view["show"].asString() != "cycle"))) {
            return Error{ErrorCode::Validation, "show must be self, stages, or cycle", child_path(view_path, "show")};
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
        return Error{ErrorCode::Validation, "rule must contain at most one operation", path};
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
        if (value.isMember("target") && !integer(value["target"])) {
            return Error{ErrorCode::Validation, "expected an int64 JSON integer", child_path(path, "target")};
        }
        if (value.isMember("target") && value["target"].asInt64() < 1) {
            return Error{ErrorCode::Validation, "Fact target must be at least 1", child_path(path, "target")};
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
            return Error{ErrorCode::Validation, "expected an object", child_path(path, "count")};
        }
        const auto count_path = child_path(path, "count");
        if (auto failure = reject_unknown_fields(count, count_path, {"target", "of"})) {
            return failure;
        }
        if (!count.isMember("target") || !integer(count["target"])) {
            return Error{ErrorCode::Validation, "expected an int64 JSON integer", child_path(count_path, "target")};
        }
        if (!count.isMember("of")) {
            return Error{ErrorCode::Validation, "missing required field 'of'", child_path(count_path, "of")};
        }
        return validate_children(count["of"], child_path(count_path, "of"));
    }

    if (auto failure = reject_unknown_fields(value, path, {"id", operation, "view"})) {
        return failure;
    }
    return validate_children(value[std::string(operation)], child_path(path, operation));
}

std::optional<Error> validate_document_shape(const Json::Value& root) {
    if (!root.isObject()) {
        return Error{ErrorCode::Validation, "template root must be an object", ""};
    }
    if (auto failure = reject_unknown_fields(root, "", {"name_key", "minecraft", "goals", "completion_rule"})) {
        return failure;
    }
    if (!root.isMember("goals") || !root["goals"].isArray()) {
        return Error{ErrorCode::Validation, "expected an array", "/goals"};
    }
    if (!root.isMember("name_key")) {
        return Error{ErrorCode::Validation, "missing required field 'name_key'", "/name_key"};
    }
    if (auto failure = check_string(root["name_key"], "/name_key")) {
        return failure;
    }
    if (!root.isMember("minecraft") || !root["minecraft"].isObject()) {
        return Error{ErrorCode::Validation, "expected an object", "/minecraft"};
    }
    const auto& minecraft = root["minecraft"];
    if (auto failure = reject_unknown_fields(minecraft, "/minecraft", {"min_version", "max_version"})) {
        return failure;
    }
    for (const auto field : {"min_version", "max_version"}) {
        if (!minecraft.isMember(field) || minecraft[field].type() != Json::intValue || minecraft[field].asInt64() < 0) {
            return Error{ErrorCode::Validation, std::string(field) + " must be a non-negative integer",
                         child_path("/minecraft", field)};
        }
    }
    if (minecraft["max_version"].asInt64() < minecraft["min_version"].asInt64()) {
        return Error{ErrorCode::Validation, "minecraft min_version must not exceed max_version", "/minecraft"};
    }
    if (!root.isMember("completion_rule")) {
        return Error{ErrorCode::Validation, "missing required field 'completion_rule'", "/completion_rule"};
    }
    if (auto failure = check_string(root["completion_rule"], "/completion_rule")) {
        return failure;
    }

    for (Json::ArrayIndex index = 0; index < root["goals"].size(); ++index) {
        if (auto failure = validate_rule_shape(root["goals"][index], "/goals/" + std::to_string(index))) {
            return failure;
        }
    }
    return std::nullopt;
}

class Diagnostics {
public:
    void add(std::string message, std::string context) {
        errors_.push_back(Error{ErrorCode::Validation, std::move(message), std::move(context)});
    }
    [[nodiscard]] bool empty() const {
        return errors_.empty();
    }
    TemplateErrors take() {
        return std::move(errors_);
    }

private:
    TemplateErrors errors_;
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
            presentation.enchanted = view.get("enchanted", false).asBool();
            if (view.isMember("state_style")) {
                if (auto failure = read_state_styles(view["state_style"], presentation.state_style,
                                                     child_path(view_path, "state_style")))
                    diagnostics.add(failure->message, failure->context);
            }
            if (view.isMember("type")) {
                const auto type = view["type"].asString();
                presentation.frame_type = type == "goal"        ? Presentation::FrameType::Goal
                                          : type == "challenge" ? Presentation::FrameType::Challenge
                                                                : Presentation::FrameType::Task;
            }
            if (presentation.localization_key.empty()) {
                diagnostics.add("view name_key must not be empty", child_path(view_path, "name_key"));
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
        if (auto& presentation = compiled.presentation_by_node[index]) {
            const auto op = compiled.graph.nodes[index].op;
            const auto& view = value["view"];
            presentation->show = op == RuleOp::Prefix ? Presentation::Show::Stages : Presentation::Show::Self;
            if (view.isMember("show")) {
                const auto show = view["show"].asString();
                presentation->show = show == "stages"  ? Presentation::Show::Stages
                                     : show == "cycle" ? Presentation::Show::Cycle
                                                       : Presentation::Show::Self;
            }
            const auto show_path = child_path(child_path(path, "view"), "show");
            if (presentation->show == Presentation::Show::Stages && op != RuleOp::Prefix && op != RuleOp::All) {
                diagnostics.add("show stages requires a prefix or all rule", show_path);
            } else if (presentation->show == Presentation::Show::Cycle && op != RuleOp::All && op != RuleOp::Any &&
                       op != RuleOp::Count) {
                diagnostics.add("show cycle requires an all, any, or count rule", show_path);
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

    // Depth-first post-order, so every dependency is evaluated before the rules that use it.
    void build_evaluation_order() {
        std::vector<std::uint8_t> state(compiled.graph.nodes.size());  // 0 new, 1 visiting, 2 done
        std::vector<std::uint32_t> stack;
        const auto visit = [&](const auto& self, const std::uint32_t index) -> void {
            if (state[index] == 2)
                return;
            if (state[index] == 1) {
                std::string message = "dependency cycle: ";
                for (auto it = std::find(stack.begin(), stack.end(), index); it != stack.end(); ++it)
                    message += label(*it) + " -> ";
                diagnostics.add(message + label(index), paths[index]);
                return;
            }
            state[index] = 1;
            stack.push_back(index);
            const auto& node = compiled.graph.nodes[index];
            for (const auto child : node.children)
                self(self, child);
            if (node.op == RuleOp::Ref && node.reference_node != no_index)
                self(self, node.reference_node);
            stack.pop_back();
            state[index] = 2;
            compiled.graph.evaluation_order.push_back(index);
        };
        for (std::uint32_t root = 0; root < compiled.graph.nodes.size(); ++root)
            visit(visit, root);
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
