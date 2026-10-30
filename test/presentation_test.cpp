#include "temporary_directory.hpp"

#include <beacon/io/file.hpp>
#include <beacon/ui/display.hpp>
#include <beacon/ui/format.hpp>
#include <beacon/ui/language.hpp>
#include <beacon/ui/progress.hpp>
#include <beacon/ui/carousel.hpp>
#include <beacon/ui/layout_metrics.hpp>
#include <beacon/app/persistence.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <limits>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

constexpr std::string_view presented_template = R"json({
  "name_key": "template.presentation",
  "minecraft": {"min_version": 3463, "max_version": 4903},
  "goals": [
    {"id":"shown","fact":"a","view":{"name_key":"goal.shown","icon":"icons/a.bmp"}},
    {"id":"hidden","all":[{"fact":"b"}],"view":{"name_key":"goal.hidden"}}
  ],
  "completion_rule": "shown"
})json";

std::string replace_once(std::string value, std::string_view from, std::string_view to) {
    value.replace(value.find(from), from.size(), to);
    return value;
}

}  // namespace

TEST_CASE("resource reload preserves independent identities and language changes only replace text") {
    const beacon::test::TemporaryDirectory temporary;
    const auto& root = temporary.path;
    std::filesystem::create_directories(root / "icons");
    std::filesystem::create_directories(root / "langs");
    const auto path = root / "template.json";
    std::ofstream(path) << presented_template;
    std::ofstream(root / "lang.json") << R"({"goal.shown":"Shown"})";
    std::ofstream(root / "langs/zh-CN.json") << R"({"goal.shown":"已翻译"})";
    std::ofstream(root / "layout.json") << R"({"main":[],"overlay":[]})";
    std::ofstream(root / "icons/a.bmp") << "image";
    beacon::TemplateResourceLoader loader;
    const auto initial = loader.load(path, root, "");
    REQUIRE(initial);
    const auto unchanged = loader.load(path, root, "");
    REQUIRE(unchanged);
    REQUIRE(unchanged->compiled == initial->compiled);
    REQUIRE(unchanged->layout == initial->layout);
    REQUIRE(unchanged->localization == initial->localization);
    beacon::TemplateResourceLoader seeded(path, root, *initial);
    const auto first_switch = seeded.load(path, root, "zh-CN", true);
    REQUIRE(first_switch);
    REQUIRE(first_switch->compiled == initial->compiled);
    REQUIRE(first_switch->layout == initial->layout);
    const auto translated = loader.load(path, root, "zh-CN", true);
    REQUIRE(translated);
    REQUIRE(translated->compiled == initial->compiled);
    REQUIRE(translated->layout == initial->layout);
    REQUIRE(translated->localization->text("goal.shown") == "已翻译");
    REQUIRE(translated->localization != initial->localization);

    std::ofstream(root / "lang.json") << R"({"goal.shown":"Reloaded"})";
    std::ofstream(root / "layout.json")
        << R"({"main":[{"id":"main","source":"nodes","nodes":["shown"],"exclude":[],"x":0,"y":0,"width":1,"height":1,"margin":0,"padding":0}],"overlay":[]})";
    const auto full_reload = loader.load(path, root, "");
    REQUIRE(full_reload);
    REQUIRE(full_reload->layout != translated->layout);
    REQUIRE(full_reload->layout->main.size() == 1);
    REQUIRE(full_reload->localization != translated->localization);
    REQUIRE(full_reload->localization->text("goal.shown") == "Reloaded");

    std::ofstream(path) << replace_once(std::string(presented_template), R"("fact":"a")", R"("fact":"c")");
    const auto changed_rules = loader.load(path, root, "zh-CN");
    REQUIRE(changed_rules);
    REQUIRE(changed_rules->compiled != translated->compiled);
    REQUIRE(changed_rules->layout == full_reload->layout);
    REQUIRE(*beacon::icon_file_stamps(*initial->compiled) == *beacon::icon_file_stamps(*changed_rules->compiled));
    const auto icon_time = std::filesystem::last_write_time(root / "icons/a.bmp");
    std::ofstream(root / "icons/replacement.bmp") << "other";
    std::filesystem::last_write_time(root / "icons/replacement.bmp", icon_time);
    std::filesystem::rename(root / "icons/a.bmp", root / "icons/old.bmp");
    std::filesystem::rename(root / "icons/replacement.bmp", root / "icons/a.bmp");
    const auto changed_image = loader.load(path, root, "zh-CN");
    REQUIRE(changed_image);
    REQUIRE(changed_image->compiled != changed_rules->compiled);
    REQUIRE(changed_image->compiled->same_rules(*changed_rules->compiled));
    REQUIRE(changed_image->layout == changed_rules->layout);
    REQUIRE(changed_image->localization == changed_rules->localization);
    std::ofstream(root / "langs/zh-CN.json") << "invalid";
    REQUIRE_FALSE(loader.load(path, root, "zh-CN", true));
    REQUIRE(changed_image->localization->text("goal.shown") == "已翻译");
}

TEST_CASE("statistic stages advance in one fixed slot and keep the final stage") {
    const auto compiled_value = beacon::compile_template_json(R"({
      "name_key":"stages", "minecraft":{"min_version":1,"max_version":1},
      "goals":[
        {"id":"chain","prefix":[
          {"fact":"stat/first","target":3},{"fact":"stat/second","target":1},{"fact":"stat/third","target":1}]},
        {"id":"ordinary","fact":"stat/other","target":2}
      ], "completion_rule":"chain"
    })");
    REQUIRE(compiled_value);
    const auto compiled_ptr = std::make_shared<const beacon::CompiledTemplate>(*compiled_value);
    const auto& compiled = *compiled_ptr;
    auto layout = std::make_shared<beacon::Layout>();
    layout->main.push_back({"Stats", beacon::LayoutSource::Nodes, {"chain", "ordinary"}, {}});
    layout->overlay = layout->main;
    beacon::PublishedState state{
        .compiled = compiled_ptr, .localization = std::make_shared<const beacon::Localization>(), .layout = layout};
    const auto slot = compiled.graph.index_by_id.at("chain");
    const auto ordinary = compiled.graph.index_by_id.at("ordinary");
    const auto& stages = compiled.graph.nodes[slot].children;
    REQUIRE(stages.size() == 3);
    beacon::Facts facts;
    beacon::ProgressViewModel view;
    const auto check_stage = [&](const std::size_t stage, const std::string& progress, const bool done) {
        const auto results = beacon::evaluate(compiled, facts);
        REQUIRE(results);
        state.snapshot.results = *results;
        view.update(state);
        const auto displayed = beacon::stat_display_node(slot, compiled, state.snapshot.results);
        REQUIRE(displayed == stages[stage]);
        CHECK(beacon::format_rule_progress(state.snapshot.results[displayed]) == progress);
        CHECK(state.snapshot.results[slot].done == done);
    };
    check_stage(0, "0/3", false);
    CHECK(view.main_stats == std::vector<bool>{true});
    CHECK(view.overlay_stats == std::vector<bool>{true});
    const auto original_main = view.main_groups;

    facts["stat/first"] = 2;
    check_stage(0, "2/3", false);
    beacon::ManualProgress manual;
    auto displayed = beacon::stat_display_node(slot, compiled, state.snapshot.results);
    REQUIRE(beacon::apply_manual_operation(compiled, state.snapshot.results, manual, displayed, true));
    CHECK(beacon::stat_display_node(slot, compiled, state.snapshot.results) == stages[1]);

    facts["stat/first"] = 3;
    check_stage(1, "0/1", false);
    facts["stat/second"] = 1;
    check_stage(2, "0/1", false);
    displayed = beacon::stat_display_node(slot, compiled, state.snapshot.results);
    manual = {};
    REQUIRE(beacon::apply_manual_operation(compiled, state.snapshot.results, manual, displayed, true));
    CHECK(state.snapshot.results[slot].done);
    CHECK(beacon::stat_display_node(slot, compiled, state.snapshot.results) == stages[2]);
    state.snapshot.results = *beacon::evaluate(compiled, facts);
    REQUIRE(beacon::apply_manual_operation(compiled, state.snapshot.results, manual, displayed, false));
    CHECK_FALSE(state.snapshot.results[slot].done);

    facts["stat/third"] = 1;
    check_stage(2, "1/1", true);
    CHECK(view.main_groups == original_main);
    CHECK(beacon::stat_display_node(ordinary, compiled, state.snapshot.results) == ordinary);
    CHECK(beacon::stat_manual_node(ordinary, false, compiled, state.snapshot.results) == ordinary);

    // Finished chains only take back the final stage.
    CHECK_FALSE(beacon::stat_manual_node(slot, true, compiled, state.snapshot.results));
    CHECK(beacon::stat_manual_node(slot, false, compiled, state.snapshot.results) == stages[2]);
    // An untouched active stage reopens the previous one; partial progress is taken back first.
    facts.erase("stat/third");
    check_stage(2, "0/1", false);
    CHECK(beacon::stat_manual_node(slot, true, compiled, state.snapshot.results) == stages[2]);
    const auto reopened = beacon::stat_manual_node(slot, false, compiled, state.snapshot.results);
    REQUIRE(reopened == stages[1]);
    manual = {};
    REQUIRE(beacon::apply_manual_operation(compiled, state.snapshot.results, manual, *reopened, false));
    CHECK(beacon::stat_display_node(slot, compiled, state.snapshot.results) == stages[1]);
    facts.clear();
    facts["stat/first"] = 1;
    check_stage(0, "1/3", false);
    CHECK(beacon::stat_manual_node(slot, false, compiled, state.snapshot.results) == stages[0]);
    facts.clear();
    check_stage(0, "0/3", false);
    CHECK_FALSE(beacon::stat_manual_node(slot, false, compiled, state.snapshot.results));
    // Results that were never evaluated have no active stage to show or change.
    const std::vector<beacon::RuleResult> unevaluated(compiled.graph.nodes.size());
    CHECK(beacon::stat_display_node(slot, compiled, unevaluated) == slot);
    CHECK_FALSE(beacon::stat_manual_node(slot, true, compiled, unevaluated));

    // Later stages never count while an earlier one is missing.
    facts["stat/third"] = 1;
    check_stage(0, "0/3", false);
}

TEST_CASE("fixed statistic groups only accept stages with explicit statistic targets") {
    const auto compiled = beacon::compile_template_json(R"({
      "name_key":"stages", "minecraft":{"min_version":1,"max_version":1},
      "goals":[
        {"id":"stats","prefix":[{"fact":"stat/a","target":1},{"fact":"stat/b","target":2}]},
        {"id":"advancements","prefix":[{"fact":"adv/a"}]},
        {"id":"targetless","prefix":[{"fact":"stat/a"}]}
      ], "completion_rule":"stats"
    })");
    REQUIRE(compiled);
    auto layout = std::make_shared<beacon::Layout>();
    for (const auto id : {"stats", "advancements", "targetless"})
        layout->main.push_back({id, beacon::LayoutSource::Nodes, {id}, {}});
    layout->overlay = layout->main;
    beacon::PublishedState state{.compiled = std::make_shared<const beacon::CompiledTemplate>(*compiled),
                                 .localization = std::make_shared<const beacon::Localization>(),
                                 .layout = layout};
    const auto results = beacon::evaluate(*compiled, {});
    REQUIRE(results);
    state.snapshot.results = *results;
    beacon::ProgressViewModel view;
    view.update(state);
    CHECK(view.main_stats == std::vector<bool>{true, false, false});
    CHECK(view.overlay_stats == view.main_stats);
}

TEST_CASE("template presentation compiles without changing rule identity") {
    const auto compiled = beacon::compile_template_json(presented_template);
    REQUIRE(compiled);
    REQUIRE(compiled->template_name_key == "template.presentation");
    REQUIRE(compiled->presentation_by_node.size() == compiled->graph.nodes.size());
    REQUIRE(compiled->presentation_by_node[compiled->graph.index_by_id.at("shown")]);
    REQUIRE(compiled->presentation_by_node[0]->localization_key == "goal.shown");
    const auto typed =
        beacon::compile_template_json(replace_once(std::string(presented_template), "\"icon\":\"icons/a.bmp\"",
                                                   "\"icon\":\"icons/a.bmp\",\"type\":\"challenge\""));
    REQUIRE(typed);
    REQUIRE(typed->presentation_by_node[0]->frame_type == beacon::Presentation::FrameType::Challenge);
    REQUIRE_FALSE(compiled->presentation_by_node[2]);

    const auto relabeled =
        beacon::compile_template_json(replace_once(std::string(presented_template), "goal.shown", "goal.renamed"));
    REQUIRE(relabeled);
    REQUIRE(relabeled->same_rules(*compiled));

    const std::vector<std::string> invalid{
        replace_once(std::string(presented_template), "\"name_key\":\"goal.shown\"",
                     "\"name_key\":\"goal.shown\",\"section\":\"future\""),
        replace_once(std::string(presented_template), "\"id\":\"shown\",", ""),
        replace_once(std::string(presented_template), "\"name_key\":\"goal.shown\"", "\"name_key\":\"\""),
        replace_once(std::string(presented_template), "\"icon\":\"icons/a.bmp\"",
                     "\"icon\":\"icons/a.bmp\",\"type\":\"bad\""),
        replace_once(std::string(presented_template), "\"icon\":\"icons/a.bmp\"",
                     "\"icon\":\"icons/a.bmp\",\"unknown\":true")};
    for (const auto& json : invalid) {
        INFO(json);
        REQUIRE_FALSE(beacon::compile_template_json(json));
    }
}

TEST_CASE("localization falls back from selected language to default and then key") {
    const auto localization = beacon::compile_localization_json(
        R"({"template.presentation":"Default","goal.shown":"Shown"})", R"({"template.presentation":"Selected"})");
    REQUIRE(localization);
    REQUIRE(localization->text("template.presentation") == "Selected");
    REQUIRE(localization->text("goal.shown") == "Shown");
    REQUIRE(localization->text("missing.key") == "missing.key");
    REQUIRE_FALSE(beacon::compile_localization_json(R"({"x":1})"));
    REQUIRE_FALSE(beacon::compile_localization_json(R"({"x":"a","x":"b"})"));
}

TEST_CASE("layout validates responsive groups and keeps unknown node IDs as warnings") {
    const auto compiled = beacon::compile_template_json(presented_template);
    REQUIRE(compiled);
    const auto layout = beacon::compile_layout_json(
        R"({"main":[{"id":"main","source":"nodes","nodes":["shown","old"],"exclude":[],"x":0,"y":0,"width":1,"height":1, "margin": 0, "padding": 0}],"overlay":[]})",
        *compiled);
    REQUIRE(layout);
    REQUIRE(layout->warnings.size() == 1);
    REQUIRE(layout->warnings.front().context == "old");

    REQUIRE_FALSE(beacon::compile_layout_json(
        R"({"main":[{"id":"main","source":"nodes","nodes":[],"exclude":[],"x":-0.1,"y":0,"width":1,"height":1, "margin": 0, "padding": 0}],"overlay":[]})",
        *compiled));
    REQUIRE_FALSE(beacon::compile_layout_json(
        R"({"main":[{"id":"main","source":"nodes","nodes":[],"exclude":[],"x":0,"y":0,"width":1,"height":1, "margin": 0, "padding": 0,"columns":0}],"overlay":[]})",
        *compiled));
    REQUIRE_FALSE(beacon::compile_layout_json(
        R"({"main":[{"id":"main","source":"nodes","nodes":[],"exclude":[],"x":0,"y":0,"width":1,"height":1}],"overlay":[]})",
        *compiled));
    REQUIRE_FALSE(beacon::compile_layout_json(R"({"main":[]})", *compiled));

    std::string many = R"({"main":[)";
    for (std::size_t index = 0; index <= beacon::max_diagnostics; ++index) {
        if (index != 0) {
            many += ',';
        }
        many += "{\"id\":\"group" + std::to_string(index) + "\",\"source\":\"nodes\",\"nodes\":[\"old" +
                std::to_string(index) +
                "\"],\"exclude\":[],\"x\":0,\"y\":0,\"width\":1,\"height\":1,\"margin\":0,\"padding\":0}";
    }
    many += "],\"overlay\":[]}";
    const auto capped = beacon::compile_layout_json(many, *compiled);
    REQUIRE(capped);
    REQUIRE(capped->warnings.size() == beacon::max_diagnostics);
}

TEST_CASE("child item styles validate states fields and supported effects") {
    const auto compiled = beacon::compile_template_json(presented_template);
    REQUIRE(compiled);
    const auto parse = [&](const std::string& style, const std::string& source = "children") {
        return beacon::compile_layout_json(
            R"({"main":[{"id":"children","source":")" + source +
                R"(","nodes":["hidden"],"exclude":[],"x":0,"y":0,"width":1,"height":1,"margin":0,"padding":0,"item_style":)" +
                style + R"(}],"overlay":[]})",
            *compiled);
    };
    const auto layout = parse(
        R"({"pending":{"emphasis":"highlight"},"done":{"visibility":"blank","emphasis":"muted","decoration":"strikethrough"}})");
    REQUIRE(layout);
    const auto& styles = layout->main.front().item_style;
    CHECK(styles.pending.emphasis == beacon::ItemEmphasis::Highlight);
    CHECK_FALSE(styles.pending.blank);
    CHECK_FALSE(styles.pending.strikethrough);
    CHECK(styles.done.blank);
    CHECK(styles.done.emphasis == beacon::ItemEmphasis::Muted);
    CHECK(styles.done.strikethrough);
    const auto defaults = parse("{}");
    REQUIRE(defaults);
    CHECK(defaults->main.front().item_style == beacon::ItemStyles{});
    CHECK(defaults->main.front().item_style.pending.emphasis == beacon::ItemEmphasis::Muted);
    CHECK(defaults->main.front().item_style.done.emphasis == beacon::ItemEmphasis::Highlight);
    const auto partial = parse(R"({"done":{"decoration":"strikethrough"}})");
    REQUIRE(partial);
    CHECK(partial->main.front().item_style.pending.emphasis == beacon::ItemEmphasis::Muted);
    CHECK(partial->main.front().item_style.done.emphasis == beacon::ItemEmphasis::Highlight);
    const auto highlighted = parse(R"({"done":{"visibility":"visible","emphasis":"highlight","decoration":"none"}})");
    REQUIRE(highlighted);
    CHECK(highlighted->main.front().item_style.done == beacon::ItemStyle{});
    const auto omitted = beacon::compile_layout_json(
        R"({"main":[{"id":"children","source":"children","nodes":["hidden"],"exclude":[],"x":0,"y":0,"width":1,"height":1,"margin":0,"padding":0}],"overlay":[]})",
        *compiled);
    REQUIRE(omitted);
    CHECK(omitted->main.front().item_style == defaults->main.front().item_style);
    for (const auto* invalid :
         {"null", "[]", R"({"unknown":{}})", R"({"done":null})", R"({"done":{"visibility":"hidden"}})",
          R"({"done":{"decoration":"cross"}})", R"({"pending":{"emphasis":"normal"}})",
          R"({"done":{"emphasis":"normal"}})", R"({"done":{"emphasis":"bright"}})", R"({"done":{"emphasis":true}})",
          R"({"done":{"color":"red"}})"}) {
        CAPTURE(invalid);
        CHECK_FALSE(parse(invalid));
    }
    CHECK_FALSE(parse("{}", "nodes"));
    CHECK_FALSE(parse("{}", "prefix"));
    CHECK_FALSE(beacon::compile_layout_json(
        R"({"main":[],"overlay":[{"id":"children","source":"children","nodes":["hidden"],"exclude":[],"x":0,"y":0,"width":1,"height":1,"margin":0,"padding":0,"item_style":{}}]})",
        *compiled));
    CHECK_FALSE(beacon::compile_layout_json(
        R"({"main":[],"overlay":[{"id":"outer","source":"nodes","nodes":[],"exclude":[],"x":0,"y":0,"width":1,"height":1,"margin":0,"padding":0,"children":[{"id":"inner","source":"children","nodes":["hidden"],"exclude":[],"x":0,"y":0,"width":1,"height":1,"margin":0,"padding":0,"item_style":{}}]}]})",
        *compiled));
}

TEST_CASE("layout normalizes arbitrary positive coordinates and sizes") {
    const auto compiled = beacon::compile_template_json(presented_template);
    REQUIRE(compiled);
    const auto layout = beacon::compile_layout_json(
        R"({"main":[{"id":"main","source":"nodes","nodes":["shown"],"exclude":[],"x":100,"y":50,"width":300,"height":150, "margin": 0, "padding": 0}],"overlay":[]})",
        *compiled);
    REQUIRE(layout);
    REQUIRE(layout->main[0].x == 0.25F);
    REQUIRE(layout->main[0].y == 0.25F);
    REQUIRE(layout->main[0].width == 0.75F);
    REQUIRE(layout->main[0].height == 0.75F);
}

TEST_CASE("layout supports nested groups with parent-relative geometry") {
    const auto compiled = beacon::compile_template_json(presented_template);
    REQUIRE(compiled);
    const auto layout = beacon::compile_layout_json(
        R"({"main":[{"id":"outer","source":"nodes","nodes":[],"exclude":[],"x":0,"y":0,"width":2,"height":2, "margin": 0, "padding": 0,"children":[{"id":"inner","source":"nodes","nodes":["shown"],"exclude":[],"x":0.25,"y":0.25,"width":0.5,"height":0.5, "margin": 0, "padding": 0}]}],"overlay":[]})",
        *compiled);
    REQUIRE(layout);
    REQUIRE(layout->main.size() == 2);
    REQUIRE(layout->main[1].id == "inner");
    REQUIRE(layout->main[1].x == 0.25F);
    REQUIRE(layout->main[1].y == 0.25F);
    REQUIRE(layout->main[1].width == 0.5F);
    REQUIRE(layout->main[1].height == 0.5F);
}

TEST_CASE("presentation resolves reusable layout nodes and fallback labels") {
    const auto compiled = beacon::compile_template_json(presented_template);
    REQUIRE(compiled);
    const beacon::Localization localization{.selected = {}, .defaults = {{"goal.shown", "Shown"}}};

    REQUIRE(beacon::display_label(compiled->graph.index_by_id.at("shown"), *compiled, localization) == "Shown");
    const auto hidden = compiled->graph.index_by_id.at("hidden");
    const auto child = compiled->graph.nodes[hidden].children.front();
    REQUIRE(beacon::display_label(child, *compiled, localization) == "b");

    const beacon::LayoutGroup children{"children", beacon::LayoutSource::Children, {"hidden"}, {}, 0, 0, 1, 1};
    REQUIRE(beacon::resolve_layout_nodes(children, *compiled, compiled->graph.nodes.size()) ==
            compiled->graph.nodes[hidden].children);

    const beacon::LayoutGroup prefix{"prefix", beacon::LayoutSource::Prefix, {""}, {"hidden"}, 0, 0, 1, 1};
    const auto nodes = beacon::resolve_layout_nodes(prefix, *compiled, 2);
    REQUIRE(nodes == std::vector<std::uint32_t>{compiled->graph.index_by_id.at("shown")});
}

TEST_CASE("collection cache preserves group order exclusions and layout changes") {
    const auto compiled_value = beacon::compile_template_json(R"({
      "name_key":"collections", "minecraft":{"min_version":1,"max_version":1},
      "goals":[
        {"id":"first","all":[{"id":"a","fact":"a"},{"id":"b","fact":"b"}]},
        {"id":"second","all":[{"id":"c","fact":"c"},{"id":"d","fact":"d"}]}
      ], "completion_rule":"first"
    })");
    REQUIRE(compiled_value);
    auto compiled = std::make_shared<const beacon::CompiledTemplate>(*compiled_value);
    auto layout = std::make_shared<beacon::Layout>();
    layout->main.push_back({"collections", beacon::LayoutSource::Children, {"second", "first"}, {"c", "b"}});
    layout->overlay = layout->main;
    beacon::PublishedState state{
        .compiled = compiled, .localization = std::make_shared<const beacon::Localization>(), .layout = layout};
    state.snapshot.results.resize(compiled->graph.nodes.size());
    beacon::ProgressViewModel view;
    view.update(state);
    const auto& ids = compiled->graph.index_by_id;
    REQUIRE(view.main_groups.front() == std::vector<std::uint32_t>{ids.at("d"), ids.at("a")});
    REQUIRE(view.main_collections.front().size() == 2);
    REQUIRE(view.main_collections.front()[0].parent == ids.at("second"));
    REQUIRE(view.main_collections.front()[0].members == std::vector<std::size_t>{0});
    REQUIRE(view.main_collections.front()[1].members == std::vector<std::size_t>{1});
    REQUIRE(view.overlay_collection_starts.front() == view.main_groups.front());
    view.update(state);
    REQUIRE(view.main_collections.front().size() == 2);
    state.layout = std::make_shared<const beacon::Layout>();
    view.update(state);
    REQUIRE(view.main_collections.empty());
    REQUIRE(view.overlay_collection_starts.empty());
}

TEST_CASE("progress cache keeps resource identities alive until replacement") {
    const auto compiled = beacon::compile_template_json(R"({
      "name_key":"cache", "minecraft":{"min_version":1,"max_version":1},
      "goals":[{"id":"goal/a","fact":"a","view":{"name_key":"label"}}],
      "completion_rule":"goal/a"
    })");
    REQUIRE(compiled);
    const auto localization = beacon::compile_localization_json(R"({"label":"First"})");
    REQUIRE(localization);
    beacon::PublishedState state{.compiled = std::make_shared<const beacon::CompiledTemplate>(*compiled),
                                 .localization = std::make_shared<const beacon::Localization>(*localization),
                                 .layout = std::make_shared<const beacon::Layout>()};
    state.snapshot.results.resize(compiled->graph.nodes.size());
    beacon::ProgressViewModel view;
    view.update(state);
    REQUIRE(view.labels.front() == "First");
    const std::weak_ptr old_compiled = state.compiled;
    const std::weak_ptr old_localization = state.localization;
    const std::weak_ptr old_layout = state.layout;
    state = {};
    REQUIRE_FALSE(old_compiled.expired());
    REQUIRE_FALSE(old_localization.expired());
    REQUIRE_FALSE(old_layout.expired());

    const auto replacement = beacon::compile_localization_json(R"({"label":"Second"})");
    REQUIRE(replacement);
    state.compiled = std::make_shared<const beacon::CompiledTemplate>(*compiled);
    state.localization = std::make_shared<const beacon::Localization>(*replacement);
    state.layout = std::make_shared<const beacon::Layout>();
    state.snapshot.results.resize(compiled->graph.nodes.size());
    view.update(state);
    REQUIRE(view.labels.front() == "Second");
    REQUIRE(old_compiled.expired());
    REQUIRE(old_localization.expired());
    REQUIRE(old_layout.expired());
}

TEST_CASE("progress summary includes collection goals but excludes statistic counters") {
    const auto compiled_value = beacon::compile_template_json(R"({
      "name_key":"template.presentation",
      "minecraft":{"min_version":3463,"max_version":4903},
      "goals":[
        {"id":"goal/leaf","fact":"adv/leaf"},
        {"id":"goal/collection","all":[{"ref":"goal/leaf"}]},
        {"id":"counter/stone","fact":"stat/minecraft:mined/minecraft:stone","target":3}
      ],
      "completion_rule":"goal/collection"
    })");
    REQUIRE(compiled_value);
    auto compiled = std::make_shared<const beacon::CompiledTemplate>(*compiled_value);
    const auto localization_value = beacon::compile_localization_json(R"({})");
    const auto layout_value = beacon::compile_layout_json(R"({"main":[],"overlay":[]})", *compiled);
    REQUIRE(localization_value);
    REQUIRE(layout_value);
    beacon::PublishedState state{.run = {},
                                 .compiled = compiled,
                                 .snapshot = {},
                                 .localization = std::make_shared<const beacon::Localization>(*localization_value),
                                 .layout = std::make_shared<const beacon::Layout>(*layout_value)};
    state.snapshot.results.resize(compiled->graph.nodes.size());

    beacon::ProgressViewModel view;
    view.update(state);
    const auto stats = beacon::summarize_progress(state, view.goal_nodes);
    REQUIRE(stats.total == 2);
}

TEST_CASE("layout metrics derive collection geometry from the group and scale consistently") {
    for (const bool overlay : {false, true}) {
        for (const auto source : {beacon::LayoutSource::Nodes, beacon::LayoutSource::Children}) {
            const beacon::LayoutGroup group{.source = source, .margin = 2, .padding = 3};
            const auto normal = beacon::calculate_layout_metrics(group, overlay, 7, 300, 300, 10, 20);
            const auto doubled = beacon::calculate_layout_metrics(group, overlay, 7, 600, 600, 20, 40, 2);
            REQUIRE(doubled.rows == normal.rows);
            CHECK(doubled.content_x == Catch::Approx(normal.content_x * 2));
            CHECK(doubled.y == Catch::Approx(normal.y * 2));
            CHECK(doubled.cell_width == Catch::Approx(normal.cell_width * 2));
            CHECK(doubled.cell_height == Catch::Approx(normal.cell_height * 2));
            CHECK(normal.gap == (!overlay && source == beacon::LayoutSource::Children ? 16 : 0));
            if (overlay) {
                CHECK(normal.rows == 1);
                CHECK(normal.cell_width == (source == beacon::LayoutSource::Children ? 64 : 84));
            }
        }
    }
}

TEST_CASE("overlay group heights count frames labels and statistic lines") {
    const beacon::LayoutGroup nodes{.source = beacon::LayoutSource::Nodes, .padding = 3};
    const beacon::LayoutGroup collections{.source = beacon::LayoutSource::Children, .padding = 3};
    const float frame = 52.0F;
    CHECK(beacon::overlay_group_min_height(collections, true, true, 12) == Catch::Approx(6 + frame));
    CHECK(beacon::overlay_group_min_height(nodes, false, false, 12) == Catch::Approx(6 + frame + 2));
    CHECK(beacon::overlay_group_min_height(nodes, true, false, 12) == Catch::Approx(6 + frame + 2 + 12));
    CHECK(beacon::overlay_group_min_height(nodes, true, true, 12) == Catch::Approx(6 + frame + 2 + 12 + 13));
    CHECK(beacon::overlay_group_min_height(nodes, true, true, 24, 2) ==
          Catch::Approx(2 * beacon::overlay_group_min_height(nodes, true, true, 12)));
}

TEST_CASE("full-width overlay bands share spare height equally") {
    const beacon::LayoutGroup top{.y = 0, .width = 1};
    const beacon::LayoutGroup bottom{.y = 0.5F, .width = 1};
    CHECK(beacon::stacks_full_width({top, bottom}));
    CHECK_FALSE(beacon::stacks_full_width({top}));
    CHECK_FALSE(beacon::stacks_full_width({bottom, top}));
    CHECK_FALSE(beacon::stacks_full_width({top, beacon::LayoutGroup{.y = 0.5F, .width = 0.5F}}));

    const std::vector<float> contents{10, 30};
    const auto bands = beacon::stack_layout_bands(contents, 5, 65);
    REQUIRE(bands.size() == 2);
    CHECK(bands[0].y == 5);
    CHECK(bands[0].height == 20);
    CHECK(bands[1].y == 25);
    CHECK(bands[1].height == 40);
    // Content taller than the body keeps its height instead of shrinking.
    const auto crowded = beacon::stack_layout_bands(contents, 0, 20);
    CHECK(crowded[1].y == 10);
    CHECK(crowded[1].height == 30);
    CHECK(beacon::stack_layout_bands({}, 0, 20).empty());
}

TEST_CASE("collection grids place members below the header and in widening columns") {
    // Three rows, two reserved for the parent: member 0 sits under it, the rest fill the next column.
    const std::vector<float> widths{10, 40, 25, 5};
    const auto grid = beacon::collection_grid(30, widths, 3, 2);
    CHECK(grid.header_width == 30);
    CHECK(grid.width == 70);
    REQUIRE(grid.members.size() == 4);
    CHECK(grid.members[0] == std::pair<float, std::size_t>{0, 2});
    CHECK(grid.members[1] == std::pair<float, std::size_t>{30, 0});
    CHECK(grid.members[2] == std::pair<float, std::size_t>{30, 1});
    CHECK(grid.members[3] == std::pair<float, std::size_t>{30, 2});
    // A member wider than the header widens the first column.
    CHECK(beacon::collection_grid(30, std::vector<float>{50}, 3, 2).header_width == 50);
    const auto empty = beacon::collection_grid(30, {}, 3, 2);
    CHECK(empty.width == 30);
    CHECK(empty.members.empty());
    // A column gap separates neighbouring columns without trailing space.
    const auto spaced = beacon::collection_grid(30, widths, 3, 2, 4);
    CHECK(spaced.width == 74);
    CHECK(spaced.members[0] == std::pair<float, std::size_t>{0, 2});
    CHECK(spaced.members[1] == std::pair<float, std::size_t>{34, 0});
    CHECK(empty.width == beacon::collection_grid(30, {}, 3, 2, 4).width);
    // Zero rows behave like a single row instead of dividing by zero.
    const auto single = beacon::collection_grid(30, std::vector<float>{10, 20}, 0, 0);
    REQUIRE(single.members.size() == 2);
    CHECK(single.members[0] == std::pair<float, std::size_t>{0, 0});
    CHECK(single.members[1] == std::pair<float, std::size_t>{30, 0});
}

TEST_CASE("progress text formats game time and rule progress") {
    CHECK(beacon::format_igt(0) == "00:00:00.00");
    CHECK(beacon::format_igt(-5) == "00:00:00.00");
    CHECK(beacon::format_igt(123456) == "01:42:52.80");
    CHECK(beacon::format_rule_progress({.value = 3, .target = 0}) == "3");
    CHECK(beacon::format_rule_progress({.value = 3, .target = 8}) == "3/8");
    CHECK(beacon::format_elapsed(59) == "59s");
    CHECK(beacon::format_elapsed(3661) == "1h 01m 01s");
    CHECK(beacon::format_percentage(0.325F) == "32.5%");
}

TEST_CASE("carousel keeps active items moving and hides completed slots") {
    beacon::CarouselState carousel;
    const std::vector<std::uint32_t> nodes{1, 2};
    const std::vector<beacon::RuleResult> results{{}, {.done = false}, {.done = true}};
    beacon::update_carousel(
        carousel, nodes, results,
        [](const auto node) {
            return node == 2;
        },
        100.0F, 20.0F, 5.0F, {}, 10.0F, 10.0F, 0.1F);
    REQUIRE(carousel.items.size() >= 2);
    REQUIRE(carousel.items.front().node == 1);
    beacon::update_carousel(
        carousel, nodes, results,
        [](const auto) {
            return false;
        },
        100.0F, 20.0F, 5.0F, {}, 10.0F, 10.0F, 0.1F);
    REQUIRE(std::ranges::none_of(carousel.items, [](const auto item) {
        return item.node == 2 && item.visible;
    }));
    CHECK(carousel.source == std::vector<std::uint32_t>{1});
}

TEST_CASE("carousel keeps refill order when a completed item shifts source indices") {
    const std::vector<std::uint32_t> nodes{0, 1, 2, 3, 4};
    const std::vector<std::uint32_t> expected{1, 2, 3, 4};
    for (const bool scroll_right : {false, true}) {
        beacon::CarouselState carousel;
        std::vector<beacon::RuleResult> results(nodes.size());
        const auto inactive = [](const auto) {
            return false;
        };
        beacon::update_carousel(carousel, nodes, results, inactive, 100, 20, 5, {}, 0, 80, 0.1F, scroll_right);
        results[0].done = true;
        beacon::update_carousel(carousel, nodes, results, inactive, 100, 20, 5, {}, 0, 80, 0.1F, scroll_right);
        REQUIRE(carousel.items.size() == expected.size() + 1);
        CHECK_FALSE(carousel.items.front().visible);
        for (std::size_t index = 0; index < expected.size(); ++index)
            CHECK(carousel.items[index + 1].node == expected[index]);
    }
}

TEST_CASE("carousel preserves completed trailing slots until they scroll offscreen") {
    const std::vector<std::uint32_t> nodes{0, 1, 2, 3, 4, 5};
    const auto inactive = [](auto) {
        return false;
    };
    for (const bool scroll_right : {false, true}) {
        CAPTURE(scroll_right);
        beacon::CarouselState carousel;
        std::vector<beacon::RuleResult> results(nodes.size());
        beacon::update_carousel(carousel, nodes, results, inactive, 100, 20, 5, {}, 0, 80, 0, scroll_right);
        const auto initial = carousel.items;
        REQUIRE(initial.size() == 4);
        results[2].done = results[3].done = true;
        beacon::update_carousel(carousel, nodes, results, inactive, 100, 20, 5, {}, 0, 80, 0, scroll_right);
        REQUIRE(carousel.items.size() == initial.size());
        for (std::size_t index = 0; index < initial.size(); ++index) {
            CHECK(carousel.items[index].node == initial[index].node);
            CHECK(carousel.items[index].x == initial[index].x);
            CHECK(carousel.items[index].visible == (index < 2));
        }
        beacon::update_carousel(carousel, nodes, results, inactive, 100, 20, 5, {}, 0, 80, 0.1F, scroll_right);
        REQUIRE(carousel.items.size() == initial.size() + 1);
        CHECK(carousel.items.back().node == 4);
        const float movement = scroll_right ? 8 : -8;
        CHECK(carousel.items.back().x == Catch::Approx((scroll_right ? -20 : 100) + movement));
        for (int frame = 0; frame < 20; ++frame)
            beacon::update_carousel(carousel, nodes, results, inactive, 100, 20, 5, {}, 0, 80, 0.1F, scroll_right);
        CHECK(std::ranges::none_of(carousel.items, [](const auto& item) {
            return item.node == 2 || item.node == 3;
        }));
    }
}

TEST_CASE("carousel directions stay mirrored through group gaps completion and refilling") {
    beacon::CarouselState left;
    beacon::CarouselState right;
    const std::vector<std::uint32_t> nodes{0, 1, 2};
    const std::vector<std::uint32_t> group_starts{0, 2};
    std::vector<beacon::RuleResult> results(3);
    const auto inactive = [](auto) {
        return false;
    };
    for (int frame = 0; frame < 120; ++frame) {
        if (frame == 20)
            results[1].done = true;
        if (frame == 40)
            for (auto& result : results)
                result.done = true;
        if (frame == 60)
            for (auto& result : results)
                result.done = false;
        beacon::update_carousel(left, nodes, results, inactive, 100, 20, 5, group_starts, 10, 80, 0.1F);
        beacon::update_carousel(right, nodes, results, inactive, 100, 20, 5, group_starts, 10, 80, 0.1F, true);
        REQUIRE(left.items.size() == right.items.size());
        for (std::size_t index = 0; index < left.items.size(); ++index) {
            REQUIRE(left.items[index].node == right.items[index].node);
            REQUIRE(right.items[index].x == Catch::Approx(80.0F - left.items[index].x));
        }
    }
    beacon::update_carousel(left, nodes, results, inactive, 100, 20, 5, group_starts, 10, 80, 0.1F, true);
    REQUIRE(left.items.front().x == 80);
    REQUIRE(left.items.front().node == 0);
}

TEST_CASE("unsafe icon paths use the placeholder without exposing files outside the template") {
    const beacon::test::TemporaryDirectory temporary;
    const auto& root = temporary.path;
    std::filesystem::create_directories(root / "icons");
    {
        std::ofstream output(root / "icons/a.bmp");
        output << "fixture";
    }
    auto compiled = beacon::compile_template_json(presented_template);
    REQUIRE(compiled);
    REQUIRE(beacon::resolve_icon_paths(*compiled, root, root));
    REQUIRE(beacon::path_from_utf8(compiled->presentation_by_node[0]->icon_path).is_absolute());

    const beacon::test::TemporaryDirectory outside_directory;
    const auto outside = outside_directory.path / "outside.bmp";
    {
        std::ofstream output(outside);
        output << "outside";
    }
    std::ofstream(root / "lang.json") << "{}";
    std::ofstream(root / "layout.json") << R"({"main":[],"overlay":[]})";
    std::vector<std::string> references{"../" + outside_directory.path.filename().generic_string() + "/outside.bmp",
                                        outside.generic_string(),
                                        "../escape.png",
                                        "icons/../icons/a.bmp",
                                        "icons//a.bmp",
                                        "vender:block_a",
                                        R"(C:\\outside.bmp)",
                                        R"(\\\\server\\share\\outside.bmp)",
                                        R"(icons\\a.bmp)",
                                        R"(icons/a.bmp\u0000ignored)"};
    std::error_code ec;
    std::filesystem::create_symlink(outside, root / "icons/link.bmp", ec);
    if (!ec)
        references.emplace_back("icons/link.bmp");
    for (const auto& reference : references) {
        CAPTURE(reference);
        std::ofstream(root / "template.json")
            << replace_once(std::string(presented_template), "icons/a.bmp", reference);
        const auto resources = beacon::load_template_resources(root / "template.json", root);
        REQUIRE(resources);
        REQUIRE(resources->compiled->presentation_by_node[0]->icon_path.empty());
        const auto stamps = beacon::icon_file_stamps(*resources->compiled);
        REQUIRE(stamps);
        REQUIRE(stamps->empty());
    }
}

TEST_CASE("shared frame paths are resolved per load and missing frames stay optional") {
    const beacon::test::TemporaryDirectory temporary;
    const auto& root = temporary.path;
    const auto widgets = root / "assets/widgets";
    std::filesystem::create_directories(widgets);
    const auto frame = widgets / "task_frame_obtained.png";
    std::ofstream(frame) << "fixture";
    const auto make_template = [] {
        beacon::CompiledTemplate compiled;
        compiled.presentation_by_node.assign(3,
                                             beacon::Presentation{.frame_type = beacon::Presentation::FrameType::Task});
        return compiled;
    };
    auto compiled = make_template();
    REQUIRE(beacon::resolve_icon_paths(compiled, root, root));
    for (const auto& presentation : compiled.presentation_by_node) {
        REQUIRE(presentation->frame_obtained_path == beacon::path_to_utf8(std::filesystem::weakly_canonical(frame)));
        REQUIRE(presentation->frame_unobtained_path.empty());
    }
    std::filesystem::remove(frame);
    compiled = make_template();
    REQUIRE(beacon::resolve_icon_paths(compiled, root, root));
    for (const auto& presentation : compiled.presentation_by_node)
        REQUIRE(presentation->frame_obtained_path.empty());
}

TEST_CASE("icons resolve only to exact files inside their own template") {
    const beacon::test::TemporaryDirectory first_template;
    const beacon::test::TemporaryDirectory second_template;
    const auto& first_root = first_template.path;
    const auto& second_root = second_template.path;
    for (const auto& root : {first_root, second_root})
        std::filesystem::create_directories(root / "icons");
    std::ofstream(first_root / "icons/a.bmp") << "first";
    std::ofstream(second_root / "icons/a.bmp") << "second";

    const auto first = beacon::compile_template_json(presented_template);
    const auto second = beacon::compile_template_json(presented_template);
    REQUIRE(first);
    REQUIRE(second);
    auto first_compiled = *first;
    auto second_compiled = *second;
    REQUIRE(beacon::resolve_icon_paths(first_compiled, first_root, first_root));
    REQUIRE(beacon::resolve_icon_paths(second_compiled, second_root, first_root));
    REQUIRE(beacon::path_from_utf8(first_compiled.presentation_by_node[0]->icon_path) ==
            std::filesystem::weakly_canonical(first_root / "icons/a.bmp"));
    REQUIRE(beacon::path_from_utf8(second_compiled.presentation_by_node[0]->icon_path) ==
            std::filesystem::weakly_canonical(second_root / "icons/a.bmp"));

    const beacon::test::TemporaryDirectory program_assets;
    std::filesystem::create_directories(program_assets.path / "assets/vender/minecraft/item");
    std::filesystem::create_directories(program_assets.path / "assets/vender/icons");
    std::ofstream(program_assets.path / "assets/vender/minecraft/item/apple.png") << "global";
    std::ofstream(program_assets.path / "assets/vender/minecraft/missingno.xpm") << "fallback";
    std::ofstream(program_assets.path / "assets/vender/icons/a.png") << "extension fallback";
    std::ofstream(program_assets.path / "assets/vender/icons/missingno.xpm") << "namespace fallback";
    std::ofstream(first_root / "icons/a.png") << "png";

    for (const auto& reference :
         {"icons/missing.bmp", "icons/a", "minecraft/item/apple.png", "vender/minecraft/item/apple.png"}) {
        CAPTURE(reference);
        auto missing =
            beacon::compile_template_json(replace_once(std::string(presented_template), "icons/a.bmp", reference));
        REQUIRE(missing);
        REQUIRE(beacon::resolve_icon_paths(*missing, first_root, program_assets.path));
        REQUIRE(missing->presentation_by_node[0]->icon_path.empty());
    }

    auto empty = beacon::compile_template_json(replace_once(std::string(presented_template), "icons/a.bmp", ""));
    REQUIRE(empty);
    REQUIRE(beacon::resolve_icon_paths(*empty, first_root, program_assets.path));
    REQUIRE(empty->presentation_by_node[0]->icon_path.empty());
}

TEST_CASE("same relative icon paths remain isolated when templates are loaded or copied") {
    const beacon::test::TemporaryDirectory temporary;
    const auto first_root = temporary.path / "first";
    const auto second_root = temporary.path / "second";
    const auto copied_root = temporary.path / "copied";
    const auto write_template = [&](const std::filesystem::path& root, const char* image) {
        std::filesystem::create_directories(root / "icons");
        std::ofstream(root / "template.json") << presented_template;
        std::ofstream(root / "lang.json") << R"({"template.presentation":"Package","goal.shown":"Shown"})";
        std::ofstream(root / "layout.json") << R"({"main":[],"overlay":[]})";
        std::ofstream(root / "icons/a.bmp") << image;
    };
    write_template(first_root, "first");
    write_template(second_root, "second");

    beacon::TemplateResourceLoader loader;
    auto first = loader.load(first_root / "template.json", temporary.path, "");
    auto second = loader.load(second_root / "template.json", temporary.path, "");
    REQUIRE(first);
    REQUIRE(second);
    const auto first_icon = first_root / "icons/a.bmp";
    const auto second_icon = second_root / "icons/a.bmp";
    REQUIRE(beacon::path_from_utf8(first->compiled->presentation_by_node[0]->icon_path) ==
            std::filesystem::weakly_canonical(first_icon));
    REQUIRE(beacon::path_from_utf8(second->compiled->presentation_by_node[0]->icon_path) ==
            std::filesystem::weakly_canonical(second_icon));

    std::filesystem::copy(first_root, copied_root, std::filesystem::copy_options::recursive);
    auto copied = loader.load(copied_root / "template.json", temporary.path, "");
    REQUIRE(copied);
    REQUIRE(beacon::path_from_utf8(copied->compiled->presentation_by_node[0]->icon_path) ==
            std::filesystem::weakly_canonical(copied_root / "icons/a.bmp"));
}

TEST_CASE("template resources load as one validated package") {
    const beacon::test::TemporaryDirectory temporary;
    const auto& root = temporary.path;
    std::filesystem::create_directories(root / "icons");
    {
        std::ofstream(root / "template.json") << presented_template;
        std::ofstream(root / "lang.json") << R"({"template.presentation":"Package","goal.shown":"Shown"})";
        std::ofstream(root / "layout.json") << R"({"main":[],"overlay":[]})";
        std::ofstream(root / "icons/a.bmp") << "fixture";
    }
    const auto resources = beacon::load_template_resources(root / "template.json", root);
    REQUIRE(resources);
    REQUIRE(resources->localization->text("template.presentation") == "Package");
    REQUIRE(resources->layout->main.empty());
    REQUIRE(resources->layout->overlay.empty());
    REQUIRE(beacon::path_from_utf8(resources->compiled->presentation_by_node[0]->icon_path).is_absolute());

    beacon::TemplateResourceLoader loader(root / "template.json", root, *resources);
    std::filesystem::remove(root / "icons/a.bmp");
    std::filesystem::create_directory(root / "icons/a.bmp");
    const auto directory_icon = loader.load(root / "template.json", root, "");
    REQUIRE(directory_icon);
    REQUIRE(directory_icon->compiled->presentation_by_node[0]->icon_path.empty());
    std::filesystem::remove(root / "icons/a.bmp");
    const auto missing_icon = loader.load(root / "template.json", root, "");
    REQUIRE(missing_icon);
    REQUIRE(missing_icon->compiled->presentation_by_node[0]->icon_path.empty());
    std::ofstream(root / "icons/a.bmp") << "restored";
    const auto restored_icon = loader.load(root / "template.json", root, "");
    REQUIRE(restored_icon);
    REQUIRE(restored_icon->compiled != missing_icon->compiled);
    REQUIRE_FALSE(restored_icon->compiled->presentation_by_node[0]->icon_path.empty());

    std::filesystem::create_directories(root / "langs");
    std::ofstream(root / "langs/en.json") << R"({"template.presentation":"English"})";
    REQUIRE(beacon::available_template_languages(root / "template.json") == std::vector<std::string>{"en"});
    const auto translated = beacon::load_template_resources(root / "template.json", root, "en");
    REQUIRE(translated);
    REQUIRE(translated->localization->text("template.presentation") == "English");
    REQUIRE(translated->localization->text("goal.shown") == "Shown");
    REQUIRE_FALSE(beacon::load_template_resources(root / "template.json", root, "../en"));

    std::filesystem::remove(root / "layout.json");
    REQUIRE_FALSE(beacon::load_template_resources(root / "template.json", root));
}

TEST_CASE("settings round-trip validates fields and ranges") {
    const beacon::test::TemporaryDirectory temporary;
    const auto& root = temporary.path;
    const auto path = root / "config/settings.json";
    beacon::Persistence persistence(root);
    beacon::Settings settings;
    settings.game_root = root / ".minecraft";
    settings.template_path = "/tmp/template.json";
    settings.language = "en";
    settings.ui_language = "en";
    settings.auto_detect = true;
    settings.overlay_transparent = false;
    settings.main_window_background_color = {0.1F, 0.2F, 0.3F};
    settings.overlay_window_background_color = {0.4F, 0.5F, 0.6F};
    settings.overlay_scroll_speed = 120.0F;
    settings.overlay_scroll_right = true;
    settings.main_window_scale = 0.75F;
    settings.overlay_window_scale = 1.5F;
    REQUIRE(persistence.save_settings(settings));

    const auto loaded = persistence.load_settings();
    REQUIRE(loaded);
    REQUIRE(*loaded);
    REQUIRE((**loaded).game_root == settings.game_root);
    REQUIRE((**loaded).language == settings.language);
    REQUIRE((**loaded).ui_language == settings.ui_language);
    REQUIRE((**loaded).auto_detect == settings.auto_detect);
    REQUIRE((**loaded).overlay_transparent == settings.overlay_transparent);
    REQUIRE((**loaded).main_window_background_color == settings.main_window_background_color);
    REQUIRE((**loaded).overlay_window_background_color == settings.overlay_window_background_color);
    REQUIRE((**loaded).overlay_scroll_speed == settings.overlay_scroll_speed);
    REQUIRE((**loaded).overlay_scroll_right == settings.overlay_scroll_right);
    REQUIRE((**loaded).main_window_scale == settings.main_window_scale);
    REQUIRE((**loaded).overlay_window_scale == settings.overlay_window_scale);

    settings.language = std::string("en\0ignored", 10);
    REQUIRE_FALSE(persistence.save_settings(settings));
    settings.language = "en";
    settings.overlay_window_background_color = {0.0F, 1.0F, 0.0F};
    REQUIRE(persistence.save_settings(settings));
    REQUIRE((**persistence.load_settings()).overlay_window_background_color ==
            settings.overlay_window_background_color);
    auto backup = path;
    backup += ".bak";
    std::filesystem::remove(backup);

    settings.main_window_scale = 0.25F;
    REQUIRE_FALSE(persistence.save_settings(settings));
    settings.main_window_scale = 0.75F;

    {
        std::ofstream output(path, std::ios::trunc);
        output
            << R"({"game_root":"/tmp/game","template_path":"/tmp/template","overlay_visible":false,"overlay_scroll_speed":60,"main_window_background_color":[0.2,0.2,0.2],"overlay_window_background_color":[0,1,0]})";
    }
    REQUIRE_FALSE(persistence.load_settings());

    {
        std::ofstream output(path, std::ios::trunc);
        output << R"({"saves_root":"/tmp","worlds":[]})";
    }
    REQUIRE_FALSE(persistence.load_settings());
    {
        std::ofstream output(path, std::ios::trunc);
        output << R"({"game_root":"/tmp"})";
    }
    REQUIRE_FALSE(persistence.load_settings());
}

TEST_CASE("interface language supports Chinese and English and old settings") {
    const beacon::test::TemporaryDirectory temporary;
    beacon::Persistence persistence(temporary.path);
    beacon::Settings settings{.game_root = temporary.path, .template_path = temporary.path / "template.json"};
    REQUIRE(settings.ui_language == "zh");
    for (const auto* language : {"zh", "en"}) {
        settings.ui_language = language;
        REQUIRE(persistence.save_settings(settings));
        const auto loaded = persistence.load_settings();
        REQUIRE(loaded);
        REQUIRE(*loaded);
        REQUIRE((**loaded).ui_language == language);
        REQUIRE((**loaded).language.empty());
    }
    REQUIRE(std::string_view(beacon::interface_text("zh", "设置", "Settings")) == "设置");
    REQUIRE(std::string_view(beacon::interface_text("en", "设置", "Settings")) == "Settings");
    for (const auto* invalid : {"", "fr", "EN"}) {
        settings.ui_language = invalid;
        REQUIRE_FALSE(persistence.save_settings(settings));
    }
    const auto path = temporary.path / "config/settings.json";
    std::filesystem::remove(temporary.path / "config/settings.json.bak");
    auto json = beacon::read_bounded_file(path, "settings");
    REQUIRE(json);
    const std::string field = ",\n  \"ui_language\" : \"en\"";
    const auto offset = json->find(field);
    REQUIRE(offset != std::string::npos);
    json->erase(offset, field.size());
    {
        std::ofstream output(path);
        output << *json;
    }
    const auto loaded = persistence.load_settings();
    REQUIRE(loaded);
    REQUIRE(*loaded);
    REQUIRE((**loaded).ui_language == "zh");
    for (const auto* invalid : {"null", "true", "42", "\"fr\"", "\"en\\u0000ignored\""}) {
        std::ofstream(path) << "{\"ui_language\":" << invalid << "," << json->substr(1);
        REQUIRE_FALSE(persistence.load_settings());
    }
}
