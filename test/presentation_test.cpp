#include "temporary_directory.hpp"

#include <beacon/io/file.hpp>
#include <beacon/ui/display.hpp>
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

TEST_CASE("bundled templates and their translations load and evaluate") {
    const auto root = beacon::path_from_utf8(BEACON_TEST_ROOT);
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root / "templates")) {
        if (!entry.is_directory())
            continue;
        const auto path = entry.path() / "template.json";
        CAPTURE(path);
        auto languages = beacon::available_template_languages(path);
        languages.insert(languages.begin(), "");
        for (const auto& language : languages) {
            CAPTURE(language);
            const auto resources = beacon::load_template_resources(path, root, language);
            INFO((resources ? "resources loaded" : resources.error().message + ": " + resources.error().context));
            REQUIRE(resources);
            INFO((resources->layout->warnings.empty() ? "layout references valid"
                                                      : resources->layout->warnings.front().context));
            REQUIRE(resources->layout->warnings.empty());
            REQUIRE(beacon::evaluate(*resources->compiled, {}));
        }
        ++count;
    }
    REQUIRE(count > 0);
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
        replace_once(std::string(presented_template), "icons/a.bmp", "../escape.png"),
        replace_once(std::string(presented_template), "icons/a.bmp", "vender:block_a"),
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

TEST_CASE("carousel keeps active items moving and removes completed items") {
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
        return item.node == 2;
    }));
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
        REQUIRE(carousel.items.size() == expected.size());
        for (std::size_t index = 0; index < expected.size(); ++index)
            CHECK(carousel.items[index].node == expected[index]);
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

TEST_CASE("icon resolution keeps resources inside the canonical template root") {
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
    std::error_code ec;
    std::filesystem::create_symlink(outside, root / "icons/link.bmp", ec);
    if (!ec) {
        auto linked = beacon::compile_template_json(
            replace_once(std::string(presented_template), "icons/a.bmp", "icons/link.bmp"));
        REQUIRE(linked);
        REQUIRE_FALSE(beacon::resolve_icon_paths(*linked, root, root));
    }
}

TEST_CASE("shared frame paths are resolved per load and missing frames stay optional") {
    const beacon::test::TemporaryDirectory temporary;
    const auto& root = temporary.path;
    const auto widgets = root / "assets/ui/widget";
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

TEST_CASE("icon namespaces resolve from the program assets directory") {
    const beacon::test::TemporaryDirectory temporary;
    const auto& root = temporary.path;
    std::filesystem::create_directories(root / "assets/vender");
    std::filesystem::create_directories(root / "assets/vender/minecraft");
    std::ofstream(root / "assets/vender/block_a.png") << "fixture";
    std::ofstream(root / "assets/vender/pig.bmp") << "fixture";
    std::ofstream(root / "assets/vender/minecraft/missingno.xpm") << "fixture";

    auto compiled =
        beacon::compile_template_json(replace_once(std::string(presented_template), "icons/a.bmp", "vender/block_a"));
    REQUIRE(compiled);
    REQUIRE(beacon::resolve_icon_paths(*compiled, root, root));
    REQUIRE(beacon::path_from_utf8(compiled->presentation_by_node[0]->icon_path) ==
            std::filesystem::weakly_canonical(root / "assets/vender/block_a.png"));

    auto entity =
        beacon::compile_template_json(replace_once(std::string(presented_template), "icons/a.bmp", "vender/pig.bmp"));
    REQUIRE(entity);
    REQUIRE(beacon::resolve_icon_paths(*entity, root, root));
    REQUIRE(beacon::path_from_utf8(entity->presentation_by_node[0]->icon_path) ==
            std::filesystem::weakly_canonical(root / "assets/vender/pig.bmp"));

    auto missing =
        beacon::compile_template_json(replace_once(std::string(presented_template), "icons/a.bmp", "vender/missing"));
    REQUIRE(missing);
    REQUIRE(beacon::resolve_icon_paths(*missing, root, root));
    REQUIRE(beacon::path_from_utf8(missing->presentation_by_node[0]->icon_path) ==
            std::filesystem::weakly_canonical(root / "assets/vender/minecraft/missingno.xpm"));
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
