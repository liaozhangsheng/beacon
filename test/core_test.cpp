#include <beacon/core/model.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <numeric>
#include <string>
#include <string_view>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
    #include <sys/resource.h>
#endif

namespace {

constexpr std::string_view template_json = R"json(
{
  "name_key": "template.test",
  "minecraft": {"min_version": 3463, "max_version": 4903},
  "goals": [
    {
      "id": "final",
      "all": [
        {"id": "forward", "ref": "stages"},
        {"id": "choice", "any": [{"fact": "missing"}, {"fact": "bonus"}]},
        {"id": "counter", "count": {"target": 1, "of": [{"fact": "a"}, {"fact": "b"}]}}
      ]
    },
    {
      "id": "stages",
      "prefix": [{"fact": "first"}, {"fact": "second"}]
    }
  ],
  "completion_rule": "final"
}
)json";

bool has_code(const beacon::TemplateErrors& errors, beacon::ErrorCode code) {
    for (const auto& value : errors) {
        if (value.code == code) {
            return true;
        }
    }
    return false;
}

std::string template_document(std::string_view goals, std::string_view completion = "x") {
    return "{\"name_key\":\"template.test\",\"minecraft\":{\"min_version\":3463,\"max_version\":4903},"
           "\"goals\":" +
           std::string(goals) + ",\"completion_rule\":\"" + std::string(completion) + "\"}";
}

}  // namespace

TEST_CASE("template compiles to deterministic preorder graph and evaluates all rule operations") {
    const auto compiled = beacon::compile_template_json(template_json);
    REQUIRE(compiled);
    REQUIRE(compiled->graph.nodes.size() == 11);
    REQUIRE(compiled->graph.index_by_id.at("final") == 0);
    REQUIRE(compiled->graph.index_by_id.at("forward") == 1);
    REQUIRE(compiled->graph.index_by_id.at("stages") == 8);

    const beacon::Facts facts{{"second", 1}, {"bonus", 1}, {"a", 1}};
    const auto first = beacon::evaluate(*compiled, facts);
    const auto repeated = beacon::evaluate(*compiled, facts);
    REQUIRE(first);
    REQUIRE(repeated);
    REQUIRE(*first == *repeated);

    REQUIRE((*first)[3].value == 0);  // Missing Fact is zero.
    REQUIRE((*first)[8].value == 0);  // A later Prefix child cannot skip the first.
    REQUIRE((*first)[8].active_child == 9);
    REQUIRE((*first)[1] == (*first)[8]);  // Ref copies the complete result.
    REQUIRE_FALSE((*first)[0].done);

    auto completed_facts = facts;
    completed_facts["first"] = 1;
    const auto completed = beacon::evaluate(*compiled, completed_facts);
    REQUIRE(completed);
    REQUIRE((*completed)[8].value == 2);
    REQUIRE((*completed)[8].active_child == std::numeric_limits<std::uint32_t>::max());
    REQUIRE((*completed)[0].done);
}

TEST_CASE("template parser rejects unsafe or structurally ambiguous JSON") {
    REQUIRE_FALSE(beacon::valid_icon_reference(std::string_view("icons/a.png\0ignored", 19)));
    const std::vector<std::pair<std::string, beacon::ErrorCode>> invalid{
        {R"({"name_key":"x","name_key":"y","minecraft":{"min_version":3463,"max_version":4903},"goals":[],"completion_rule":"x"})",
         beacon::ErrorCode::Parse},
        {R"({"name_key":"template.test","minecraft":{"min_version":3463,"max_version":4903},"goals":[],"completion_rule":"x","future":true})",
         beacon::ErrorCode::Validation},
        {R"({"name_key":1,"minecraft":{"min_version":3463,"max_version":4903},"goals":[],"completion_rule":"x"})",
         beacon::ErrorCode::Validation},
        {R"({"name_key":"template.test","minecraft":{"min_version":3463,"max_version":4903},"goals":[{"id":"x","fact":"a","target":9223372036854775808}],"completion_rule":"x"})",
         beacon::ErrorCode::Validation},
        {R"({"name_key":"template.test","minecraft":{"min_version":3463,"max_version":4903},"goals":[{"id":"x","fact":"a","ref":"x"}],"completion_rule":"x"})",
         beacon::ErrorCode::Validation},
    };

    for (const auto& [json, code] : invalid) {
        const auto result = beacon::compile_template_json(json);
        REQUIRE_FALSE(result);
        REQUIRE(result.error().size() == 1);
        REQUIRE(result.error().front().code == code);
    }

    const auto max_int =
        beacon::compile_template_json(template_document(R"([{"id":"x","fact":"a","target":9223372036854775807}])"));
    if (!max_int) {
        INFO(max_int.error().front().message);
    }
    REQUIRE(max_int);

    const std::string long_key(beacon::max_string_bytes + 1, 'x');
    const auto long_string =
        beacon::compile_template_json(template_document("[{\"id\":\"x\",\"fact\":\"" + long_key + "\"}]"));
    REQUIRE_FALSE(long_string);
    REQUIRE(has_code(long_string.error(), beacon::ErrorCode::SecurityLimit));

    const auto oversized = beacon::compile_template_json(std::string(beacon::max_json_bytes + 1, ' '));
    REQUIRE_FALSE(oversized);
    REQUIRE(has_code(oversized.error(), beacon::ErrorCode::SecurityLimit));

    std::string too_many_goals = "[";
    for (std::size_t index = 0; index <= beacon::max_rule_nodes; ++index) {
        too_many_goals += index == 0 ? R"({"id":"x","fact":"a"})" : R"(,{"fact":"a"})";
    }
    too_many_goals += ']';
    const auto node_limit = beacon::compile_template_json(template_document(too_many_goals));
    REQUIRE_FALSE(node_limit);
    REQUIRE(has_code(node_limit.error(), beacon::ErrorCode::SecurityLimit));

    const auto nested = [](std::size_t array_count) {
        return "{\"x\":" + std::string(array_count, '[') + "0" + std::string(array_count, ']') + "}";
    };
    const auto depth_128 = beacon::compile_template_json(nested(beacon::max_json_depth - 1));
    REQUIRE_FALSE(depth_128);  // Parsed, then rejected because the root is not a template document.
    REQUIRE_FALSE(has_code(depth_128.error(), beacon::ErrorCode::SecurityLimit));
    const auto depth_129 = beacon::compile_template_json(nested(beacon::max_json_depth));
    REQUIRE_FALSE(depth_129);
    REQUIRE(has_code(depth_129.error(), beacon::ErrorCode::SecurityLimit));
}

TEST_CASE("targetless statistics keep their raw count") {
    const auto compiled = beacon::compile_template_json(R"({
      "name_key":"stats", "minecraft":{"min_version":1,"max_version":1},
      "goals":[
        {"id":"stat","fact":"stat/minecraft:mined/minecraft:stone"},
        {"id":"goal","fact":"a"}
      ], "completion_rule":"goal"})");
    REQUIRE(compiled);
    const auto stat = compiled->graph.index_by_id.at("stat");
    const auto goal = compiled->graph.index_by_id.at("goal");
    const auto results = beacon::evaluate(*compiled, {{"stat/minecraft:mined/minecraft:stone", 7}, {"a", 1}});
    REQUIRE(results);
    REQUIRE((*results)[stat].value == 7);
    REQUIRE((*results)[stat].target == 0);
    REQUIRE_FALSE((*results)[stat].done);
    REQUIRE((*results)[goal].done);
}

TEST_CASE("template compiler reports semantic errors without returning a partial graph") {
    const std::vector<std::string> invalid{
        template_document(R"([])"),
        template_document(R"([{"id":"","fact":"a"}])"),
        template_document(R"([{"id":"x","fact":"a"},{"id":"x","fact":"b"}])"),
        template_document(R"([{"id":"x","ref":"missing"}])"),
        template_document(R"([{"id":"a","ref":"b"},{"id":"b","ref":"a"}])", "a"),
        template_document(R"([{"id":"x","all":[]}])"),
        template_document(R"([{"id":"x","any":[]}])"),
        template_document(R"([{"id":"x","prefix":[]}])"),
        template_document(R"([{"id":"x","count":{"target":1,"of":[]}}])"),
        template_document(R"([{"id":"x","fact":"a","target":0}])"),
        template_document(R"([{"id":"x","count":{"target":2,"of":[{"fact":"a"}]}}])"),
        template_document(R"([{"fact":"a"}])", "anonymous"),
    };

    for (const auto& json : invalid) {
        INFO(json);
        const auto result = beacon::compile_template_json(json);
        REQUIRE_FALSE(result);
        REQUIRE(has_code(result.error(), beacon::ErrorCode::Validation));
    }

    std::string noisy_goals = "[";
    for (std::size_t index = 0; index <= beacon::max_diagnostics; ++index) {
        noisy_goals += index == 0 ? R"({"id":"","fact":"a"})" : R"(,{"id":"","fact":"a"})";
    }
    noisy_goals += ']';
    const auto capped = beacon::compile_template_json(template_document(noisy_goals, "missing"));
    REQUIRE_FALSE(capped);
    REQUIRE(capped.error().size() == beacon::max_diagnostics);
    REQUIRE(capped.error().back().message.find("truncated") != std::string::npos);
}

TEST_CASE("template supports manual-only leaf rules") {
    const auto compiled = beacon::compile_template_json(
        R"({"name_key":"manual","minecraft":{"min_version":1,"max_version":1},"goals":[{"id":"manual","view":{"name_key":"manual"}}],"completion_rule":"manual"})");
    REQUIRE(compiled);

    auto results = beacon::evaluate(*compiled, {});
    REQUIRE(results);
    const auto node = compiled->graph.index_by_id.at("manual");
    REQUIRE_FALSE((*results)[node].done);

    beacon::ManualProgress manual;
    REQUIRE(beacon::apply_manual_operation(*compiled, *results, manual, node, true));
    REQUIRE((*results)[node].done);
}

TEST_CASE("template compiler handles the maximum legal Ref chain without call-stack recursion") {
#if defined(__unix__) || defined(__APPLE__)
    rlimit stack_limit{};
    REQUIRE(getrlimit(RLIMIT_STACK, &stack_limit) == 0);
    stack_limit.rlim_cur = std::min<rlim_t>(stack_limit.rlim_max, 1024U * 1024U);
    REQUIRE(setrlimit(RLIMIT_STACK, &stack_limit) == 0);
#endif

    std::string chain_goals = "[";
    for (std::size_t index = 0; index < beacon::max_rule_nodes; ++index) {
        if (index != 0) {
            chain_goals += ',';
        }
        chain_goals += "{\"id\":\"n" + std::to_string(index) + "\",";
        chain_goals += index + 1 == beacon::max_rule_nodes ? "\"fact\":\"done\"}"
                                                           : "\"ref\":\"n" + std::to_string(index + 1) + "\"}";
    }
    chain_goals += ']';

    const auto compiled = beacon::compile_template_json(template_document(chain_goals, "n0"));
    REQUIRE(compiled);
    REQUIRE(compiled->graph.nodes.size() == beacon::max_rule_nodes);
    std::vector<std::uint32_t> expected_order(beacon::max_rule_nodes);
    std::iota(expected_order.rbegin(), expected_order.rend(), 0U);
    REQUIRE(compiled->graph.evaluation_order == expected_order);
}

TEST_CASE("disk and manual evaluation reject the same malformed graphs") {
    auto compiled = beacon::compile_template_json(template_json);
    REQUIRE(compiled);
    auto base = beacon::evaluate(*compiled, {});
    REQUIRE(base);

    SECTION("invalid node index") {
        compiled->graph.evaluation_order.front() = std::numeric_limits<std::uint32_t>::max();
    }
    SECTION("duplicate node") {
        compiled->graph.evaluation_order.back() = compiled->graph.evaluation_order.front();
    }
    SECTION("missing node") {
        compiled->graph.evaluation_order.pop_back();
    }
    SECTION("dependency evaluated after its parent") {
        std::reverse(compiled->graph.evaluation_order.begin(), compiled->graph.evaluation_order.end());
    }
    SECTION("invalid Prefix child after an incomplete child") {
        const auto prefix = compiled->graph.index_by_id.at("stages");
        compiled->graph.nodes[prefix].children.back() = std::numeric_limits<std::uint32_t>::max();
    }
    const auto result = beacon::evaluate(*compiled, {});
    REQUIRE_FALSE(result);
    REQUIRE(result.error().code == beacon::ErrorCode::Internal);
    const auto manual = beacon::apply_manual_progress(*compiled, *base, {});
    REQUIRE_FALSE(manual);
    REQUIRE(manual.error().code == beacon::ErrorCode::Internal);
}

TEST_CASE("manual progress updates facts and derived rules without changing facts") {
    const auto compiled = beacon::compile_template_json(R"({
      "name_key":"manual", "minecraft":{"min_version":1,"max_version":1},
      "goals":[
        {"id":"leaf","fact":"adv/leaf"},
        {"id":"counter","fact":"stat/minecraft:mined/minecraft:stone","target":3},
        {"id":"all","all":[{"ref":"leaf"},{"ref":"counter"}]}
      ], "completion_rule":"all"})");
    REQUIRE(compiled);
    const beacon::Facts facts{{"adv/leaf", 0}, {"stat/minecraft:mined/minecraft:stone", 2}};
    auto results = beacon::evaluate(*compiled, facts);
    REQUIRE(results);
    beacon::ManualProgress manual;
    const auto leaf = compiled->graph.index_by_id.at("leaf");
    const auto counter = compiled->graph.index_by_id.at("counter");
    const auto base = *results;
    REQUIRE(beacon::apply_manual_operation(*compiled, *results, manual, leaf, true));
    REQUIRE((*results)[leaf].done);
    REQUIRE_FALSE(facts.at("adv/leaf"));
    *results = base;
    REQUIRE(beacon::apply_manual_operation(*compiled, *results, manual, counter, true));
    REQUIRE((*results)[counter].value == 3);
    REQUIRE((*results)[compiled->completion_node].done);
    *results = base;
    REQUIRE(beacon::apply_manual_operation(*compiled, *results, manual, counter, false));
    REQUIRE((*results)[counter].value == 2);
    REQUIRE_FALSE((*results)[compiled->completion_node].done);
    *results = base;
    REQUIRE(beacon::apply_manual_operation(*compiled, *results, manual, leaf, false));
    REQUIRE_FALSE((*results)[leaf].done);
}

TEST_CASE("snapshot publishing increments revisions and reflects current results") {
    const auto compiled = beacon::compile_template_json(template_json);
    REQUIRE(compiled);

    beacon::Facts facts{{"first", 1}, {"second", 1}, {"bonus", 1}, {"a", 1}};
    auto results = beacon::evaluate(*compiled, facts);
    REQUIRE(results);
    auto first = beacon::publish(*compiled, *results, {7, 120}, nullptr);
    REQUIRE(first);
    REQUIRE(first->revision == 1);
    REQUIRE(first->results[compiled->completion_node].done);
    REQUIRE(first->play_ticks == 120);
    REQUIRE(first->completion_play_ticks == 120);

    auto refreshed = beacon::publish(*compiled, *results, {7, 180}, &*first);
    REQUIRE(refreshed);
    REQUIRE(refreshed->completion_play_ticks == 120);

    facts.erase("first");
    results = beacon::evaluate(*compiled, facts);
    REQUIRE(results);
    auto rollback = beacon::publish(*compiled, *results, {7, 140}, &*first);
    REQUIRE(rollback);
    REQUIRE(rollback->revision == 2);
    REQUIRE_FALSE(rollback->results[compiled->completion_node].done);
    REQUIRE(rollback->play_ticks == 140);

    auto recreated = beacon::publish(*compiled, *results, {7, 20}, &*rollback);
    REQUIRE(recreated);
    REQUIRE_FALSE(recreated->results[compiled->completion_node].done);
    REQUIRE(recreated->play_ticks == 20);

    auto reset = beacon::publish(*compiled, *results, {8, 150}, &*rollback);
    REQUIRE(reset);
    REQUIRE(reset->revision == 3);
    REQUIRE_FALSE(reset->results[compiled->completion_node].done);
    REQUIRE(reset->play_ticks == 150);

    REQUIRE_FALSE(beacon::publish(*compiled, {}, {8, 150}, &*reset));
    REQUIRE_FALSE(beacon::publish(*compiled, *results, {8, -1}, &*reset));

    reset->revision = std::numeric_limits<std::uint64_t>::max();
    REQUIRE_FALSE(beacon::publish(*compiled, *results, {8, 160}, &*reset));
}
