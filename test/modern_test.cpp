#include "temporary_directory.hpp"

#include <beacon/app/runtime.hpp>
#include <beacon/app/persistence.hpp>
#include <beacon/io/file.hpp>
#include <beacon/ui/display.hpp>
#include <beacon/ui/progress.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

std::filesystem::path fixture(std::string_view relative) {
    return beacon::path_from_utf8(BEACON_TEST_ROOT) / "test/fixtures/modern" / relative;
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void write_text(const std::filesystem::path& path, std::string_view text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
}

beacon::RunSelection selection(const std::filesystem::path& world,
                               std::shared_ptr<const beacon::CompiledTemplate> compiled,
                               std::string world_key = "world-a") {
    beacon::VersionProfile profile{.data_version = std::nullopt};
    const auto localization = beacon::compile_localization_json(R"({"template.modern_test":"Modern test"})");
    const auto layout = beacon::compile_layout_json(R"({"main":[],"overlay":[]})", *compiled);
    REQUIRE(localization);
    REQUIRE(layout);
    return {{{"Fixture world", std::move(world_key)}, world},
            {"player-one"},
            std::move(compiled),
            std::move(profile),
            std::make_shared<const beacon::Localization>(*localization),
            std::make_shared<const beacon::Layout>(*layout),
            {}};
}

void process_until_published(beacon::Runtime& runtime) {
    for (int attempt = 0; attempt < 4; ++attempt) {
        const auto result = runtime.process_next(std::chrono::seconds(2));
        REQUIRE(result);
        if (*result) {
            return;
        }
    }
    FAIL("current worker generation was not published");
}

}  // namespace

TEST_CASE("run identity survives JSON replacement but resets on world replacement rollback and explicit restart") {
    const beacon::test::TemporaryDirectory temporary;
    const auto& root = temporary.path;
    const auto world = root / "saves/world";
    const auto stats = world / "stats/player-one.json";
    const auto advancements = world / "advancements/player-one.json";
    const auto populate = [&] {
        std::filesystem::create_directories(stats.parent_path());
        std::filesystem::create_directories(advancements.parent_path());
        write_text(stats, read_text(fixture("world/stats/player-one.json")));
        write_text(advancements, "{}");
    };
    populate();
    const auto compiled = beacon::compile_template_json(read_text(fixture("template.json")));
    REQUIRE(compiled);
    beacon::Runtime runtime;
    REQUIRE(runtime.select(selection(world, std::make_shared<const beacon::CompiledTemplate>(*compiled))));
    process_until_published(runtime);
    const auto first_epoch = runtime.state()->snapshot.run_epoch;
    REQUIRE(runtime.manual_operation(2, true));

    // Minecraft can atomically replace JSON files without creating a new world.
    std::filesystem::rename(stats, root / "previous-stats.json");
    auto later = read_text(fixture("world/stats/player-one.json"));
    later.replace(later.find("120"), 3, "200");
    write_text(stats, later);
    REQUIRE(runtime.poll_files());
    process_until_published(runtime);
    REQUIRE(runtime.state()->snapshot.run_epoch == first_epoch);
    REQUIRE(runtime.state()->snapshot.results[2].done);

    // Same path and same play time, but a different world directory.
    std::filesystem::rename(world, root / "previous-world");
    populate();
    write_text(stats, later);
    REQUIRE(runtime.poll_files());
    process_until_published(runtime);
    REQUIRE(runtime.state()->snapshot.run_epoch == first_epoch + 1);
    REQUIRE_FALSE(runtime.state()->snapshot.results[2].done);

    REQUIRE(runtime.manual_operation(2, true));
    write_text(stats, read_text(fixture("world/stats/player-one.json")));
    REQUIRE(runtime.poll_files());
    process_until_published(runtime);
    REQUIRE(runtime.state()->snapshot.run_epoch == first_epoch + 2);
    REQUIRE_FALSE(runtime.state()->snapshot.results[2].done);

    REQUIRE(runtime.manual_operation(2, true));
    REQUIRE(runtime.poll_files());
    REQUIRE(runtime.restart_run());
    REQUIRE(runtime.state()->data_status == beacon::DataStatus::Unavailable);
    process_until_published(runtime);
    REQUIRE(runtime.state()->snapshot.run_epoch == first_epoch + 3);
    REQUIRE_FALSE(runtime.state()->snapshot.results[2].done);
}

TEST_CASE("read failures publish stale health without losing progress and recovery clears the error") {
    const beacon::test::TemporaryDirectory temporary;
    const auto& root = temporary.path;
    std::filesystem::copy(fixture("world"), root / "world", std::filesystem::copy_options::recursive);
    const auto compiled = beacon::compile_template_json(read_text(fixture("template.json")));
    REQUIRE(compiled);
    beacon::Runtime runtime;
    REQUIRE(runtime.select(selection(root / "world", std::make_shared<const beacon::CompiledTemplate>(*compiled))));
    process_until_published(runtime);
    REQUIRE(runtime.manual_operation(1, true));
    const auto before = runtime.state();
    const auto stats = root / "world/stats/player-one.json";
    write_text(stats, "{}");
    REQUIRE(runtime.poll_files());
    REQUIRE_FALSE(runtime.process_next(std::chrono::seconds(2)));
    const auto stale = runtime.state();
    REQUIRE(stale != before);
    REQUIRE(stale->data_status == beacon::DataStatus::Stale);
    REQUIRE(stale->error);
    REQUIRE(beacon::is_complete(*before));
    REQUIRE_FALSE(beacon::is_complete(*stale));
    REQUIRE(stale->snapshot.results == before->snapshot.results);
    REQUIRE(stale->snapshot.revision == before->snapshot.revision);
    REQUIRE(stale->last_successful_read_at == before->last_successful_read_at);
    REQUIRE_FALSE(runtime.manual_operation(1, true));
    REQUIRE(runtime.configure({}, stale->compiled, stale->localization, stale->layout));
    REQUIRE(runtime.state()->data_status == beacon::DataStatus::Stale);
    REQUIRE(runtime.state()->error == stale->error);
    write_text(stats, read_text(fixture("world/stats/player-one.json")));
    REQUIRE(runtime.poll_files());
    process_until_published(runtime);
    REQUIRE(runtime.state()->data_status == beacon::DataStatus::Ready);
    REQUIRE_FALSE(runtime.state()->error);
    REQUIRE(runtime.state()->snapshot.run_epoch == before->snapshot.run_epoch);
}

TEST_CASE("discovery isolates unrelated I/O failures and invalidates cached version decisions") {
    const beacon::test::TemporaryDirectory temporary;
    const auto& root = temporary.path;
    const auto active_world = root / "saves/active";
    std::filesystem::create_directories(root / "saves");
    std::filesystem::copy(fixture("world"), active_world, std::filesystem::copy_options::recursive);
    const auto compiled = beacon::compile_template_json(read_text(fixture("template.json")));
    REQUIRE(compiled);
    beacon::SourceDiscovery discovery;
    auto current = discovery.scan(root, nullptr, &*compiled);
    REQUIRE(current);
    std::filesystem::create_directories(root / "saves/broken");
    write_text(root / "saves/broken/stats", "not a directory");
    REQUIRE(discovery.scan(root, &*current, &*compiled));
    REQUIRE_FALSE(discovery.warnings.empty());

    const auto newer = root / "saves/newer/stats/player-one.json";
    std::filesystem::create_directories(newer.parent_path());
    auto incompatible = read_text(fixture("world/stats/player-one.json"));
    incompatible.replace(incompatible.find("4189"), 4, "9999");
    write_text(newer, incompatible);
    const auto time = std::filesystem::file_time_type::clock::now() + std::chrono::hours(1);
    std::filesystem::last_write_time(newer, time);
    auto detected = discovery.scan(root, &*current, &*compiled);
    REQUIRE(detected);
    REQUIRE(detected->world.path == active_world);
    // Unchanged files reuse their validated version verdict.
    detected = discovery.scan(root, &*current, &*compiled);
    REQUIRE(detected);
    REQUIRE(discovery.warnings.size() == 1);
    write_text(newer, read_text(fixture("world/stats/player-one.json")));
    std::filesystem::last_write_time(newer, time + std::chrono::seconds(1));
    detected = discovery.scan(root, &*current, &*compiled);
    REQUIRE(detected);
    REQUIRE(detected->world.path == root / "saves/newer");
    current = detected;
    write_text(newer, incompatible);
    std::filesystem::last_write_time(newer, time + std::chrono::seconds(2));
    detected = discovery.scan(root, &*current, &*compiled);
    REQUIRE(detected);
    REQUIRE(detected->world.path == active_world);
}

TEST_CASE("player documents cache independently and retry failed reads even when metadata is restored") {
    const beacon::test::TemporaryDirectory temporary;
    const auto& root = temporary.path;
    std::filesystem::copy(fixture("world"), root / "world", std::filesystem::copy_options::recursive);
    const beacon::MinecraftPaths paths{root / "world/stats/player-one.json",
                                       root / "world/advancements/player-one.json"};
    beacon::MinecraftReader reader;
    auto first = reader.read(paths);
    REQUIRE(first);
    REQUIRE(*first);
    const auto stats_text = read_text(paths.stats);
    const auto stats_time = std::filesystem::last_write_time(paths.stats);
    write_text(paths.advancements, "{}");
    auto changed = reader.read(paths);
    REQUIRE(changed);
    REQUIRE(*changed);
    REQUIRE((**changed).facts.at("clock/play_ticks") == 120);
    REQUIRE_FALSE((**changed).facts.contains("adv/minecraft:story/mine_stone"));
    // A same-size corrupt rewrite must not hide behind a restored mtime.
    write_text(paths.stats, std::string(stats_text.size(), 'x'));
    std::filesystem::last_write_time(paths.stats, stats_time);
    REQUIRE_FALSE(reader.read(paths));
    write_text(paths.stats, stats_text);
    std::filesystem::last_write_time(paths.stats, stats_time);
    auto recovered = reader.read(paths);
    REQUIRE(recovered);
    REQUIRE(*recovered);
    REQUIRE_FALSE(*reader.read(paths));
}

TEST_CASE("file caches notice replacements with unchanged size and modification time") {
    const beacon::test::TemporaryDirectory temporary;
    const auto& root = temporary.path;
    const auto world = root / "saves/world";
    std::filesystem::create_directories(world.parent_path());
    std::filesystem::copy(fixture("world"), world, std::filesystem::copy_options::recursive);
    const beacon::MinecraftPaths paths{world / "stats/player-one.json", world / "advancements/player-one.json"};
    const auto compiled = beacon::compile_template_json(read_text(fixture("template.json")));
    REQUIRE(compiled);
    beacon::MinecraftReader reader;
    REQUIRE(reader.read(paths));
    beacon::SourceDiscovery discovery;
    const auto current = discovery.scan(root, nullptr, &*compiled);
    REQUIRE(current);

    const auto replace = [&](const std::filesystem::path& path, std::string text) {
        REQUIRE(text.size() == std::filesystem::file_size(path));
        const auto time = std::filesystem::last_write_time(path);
        const auto replacement = root / "replacement.json";
        write_text(replacement, text);
        std::filesystem::last_write_time(replacement, time);
        std::filesystem::rename(path, root / "previous.json");
        std::filesystem::rename(replacement, path);
        std::filesystem::remove(root / "previous.json");
    };
    auto stats = read_text(paths.stats);
    stats.replace(stats.find("120"), 3, "200");
    replace(paths.stats, stats);
    auto refreshed = reader.read(paths);
    REQUIRE(refreshed);
    REQUIRE(*refreshed);
    REQUIRE((**refreshed).facts.at("clock/play_ticks") == 200);
    REQUIRE_FALSE(*reader.read(paths));

    stats.replace(stats.find("4189"), 4, "9999");
    replace(paths.stats, stats);
    REQUIRE_FALSE(discovery.scan(root, &*current, &*compiled));
}

TEST_CASE("runtime publishes warnings with data errors and unchanged refreshes") {
    const beacon::test::TemporaryDirectory temporary;
    const auto& root = temporary.path;
    const auto world = root / "saves/world";
    std::filesystem::create_directories(world.parent_path());
    std::filesystem::copy(fixture("world"), world, std::filesystem::copy_options::recursive);
    const auto compiled = beacon::compile_template_json(read_text(fixture("template.json")));
    REQUIRE(compiled);
    beacon::Runtime runtime;
    auto selected = selection(world, std::make_shared<const beacon::CompiledTemplate>(*compiled));
    selected.game_root = root;
    REQUIRE(runtime.select(std::move(selected)));
    process_until_published(runtime);
    const auto original = runtime.state();

    std::filesystem::create_directories(root / "saves/broken");
    write_text(root / "saves/broken/stats", "not a directory");
    REQUIRE(runtime.poll_files(true));
    process_until_published(runtime);
    REQUIRE(runtime.state()->warnings.size() == 1);
    REQUIRE(original->warnings.empty());
    REQUIRE(runtime.state()->snapshot.revision == original->snapshot.revision);
    const auto warnings = runtime.state()->warnings;

    const auto stats = world / "stats/player-one.json";
    auto later = read_text(stats);
    later.replace(later.find("120"), 3, "200");
    write_text(stats, later);
    REQUIRE(runtime.poll_files(true));
    process_until_published(runtime);
    REQUIRE(runtime.state()->snapshot.play_ticks == 200);
    REQUIRE(runtime.state()->warnings == warnings);
    REQUIRE(original->snapshot.play_ticks == 120);
    const auto before_manual = runtime.state();
    REQUIRE(runtime.manual_operation(1, true));
    REQUIRE(runtime.state()->warnings == warnings);
    REQUIRE(runtime.state()->last_successful_read_at == before_manual->last_successful_read_at);
    REQUIRE(runtime.state()->run.player.uuid == before_manual->run.player.uuid);
    REQUIRE(runtime.state()->snapshot.revision == before_manual->snapshot.revision + 1);
    REQUIRE(runtime.state()->snapshot.results[1].value == before_manual->snapshot.results[1].value + 1);
    REQUIRE(before_manual->snapshot.results[1].value == 3);
    const auto ready = runtime.state();

    write_text(stats, "{}");
    REQUIRE(runtime.poll_files(true));
    REQUIRE_FALSE(runtime.process_next(std::chrono::seconds(2)));
    REQUIRE(runtime.state()->data_status == beacon::DataStatus::Stale);
    REQUIRE(runtime.state()->warnings == warnings);
    REQUIRE(runtime.state()->snapshot.results == ready->snapshot.results);
    std::filesystem::remove_all(root / "saves/broken");
    REQUIRE(runtime.poll_files(true));
    REQUIRE_FALSE(runtime.process_next(std::chrono::seconds(2)));
    REQUIRE(runtime.state()->warnings.empty());
    REQUIRE(runtime.state()->error);

    write_text(stats, later);
    REQUIRE(runtime.poll_files(true));
    process_until_published(runtime);
    REQUIRE(runtime.state()->data_status == beacon::DataStatus::Ready);
    REQUIRE(runtime.state()->warnings.empty());
    REQUIRE_FALSE(runtime.state()->error);
    const auto recovered = runtime.state();
    REQUIRE(runtime.poll_files(true));
    const auto unchanged = runtime.process_next(std::chrono::seconds(2));
    REQUIRE(unchanged);
    REQUIRE_FALSE(*unchanged);
    REQUIRE(runtime.state() == recovered);
}

TEST_CASE("Modern template compares rules independently of JSON field order") {
    const auto text = read_text(fixture("template.json"));
    const auto compiled = beacon::compile_template_json(text);
    REQUIRE(compiled);

    auto reordered = text;
    constexpr std::string_view original = R"("min_version": 3463, "max_version": 4903)";
    constexpr std::string_view replacement = R"("max_version": 4903, "min_version": 3463)";
    reordered.replace(reordered.find(original), original.size(), replacement);
    const auto same = beacon::compile_template_json(reordered);
    REQUIRE(same);
    REQUIRE(same->same_rules(*compiled));

    auto changed = text;
    changed.replace(changed.find("\"target\": 3"), std::string("\"target\": 3").size(), "\"target\": 4");
    const auto different = beacon::compile_template_json(changed);
    REQUIRE(different);
    REQUIRE(!different->same_rules(*compiled));
}

TEST_CASE("Modern fixtures produce canonical stats advancements recipes and same-source play ticks") {
    const auto stats = beacon::read_modern_stats(fixture("world/stats/player-one.json"));
    const auto advancements = beacon::read_advancements(fixture("world/advancements/player-one.json"));
    REQUIRE(stats);
    REQUIRE(advancements);
    REQUIRE(stats->data_version == 4189);
    REQUIRE(stats->facts.at("stat/minecraft:mined/minecraft:stone") == 3);
    REQUIRE(stats->facts.at("clock/play_ticks") == 120);
    REQUIRE(advancements->at("adv/minecraft:story/mine_stone") == 1);
    REQUIRE(advancements->at("criterion/minecraft:story/mine_stone/get_stone") == 1);
    REQUIRE(advancements->at("recipe/minecraft:building_blocks/oak_planks") == 1);
}

TEST_CASE("Modern stats accept the pre-1.17 play time key") {
    const beacon::test::TemporaryDirectory temporary;
    const auto& root = temporary.path;
    const auto stats_path = root / "player.json";
    write_text(stats_path, R"({"DataVersion":1519,"stats":{"minecraft:custom":{"minecraft:play_one_minute":40}}})");
    const auto stats = beacon::read_modern_stats(stats_path);
    REQUIRE(stats);
    REQUIRE(stats->facts.at("clock/play_ticks") == 40);
}

TEST_CASE("Modern stats require DataVersion") {
    const beacon::test::TemporaryDirectory temporary;
    const auto& root = temporary.path;
    const auto stats_path = root / "player.json";
    write_text(stats_path, R"({"stats":{"minecraft:custom":{"minecraft:play_time":40}}})");
    const auto stats = beacon::read_modern_stats(stats_path);
    REQUIRE_FALSE(stats);
    REQUIRE(stats.error().code == beacon::ErrorCode::Validation);
}

TEST_CASE("Minecraft JSON readers reject oversized files before parsing") {
    const beacon::test::TemporaryDirectory temporary;
    const auto& root = temporary.path;
    const auto stats_path = root / "player.json";
    {
        std::ofstream output(stats_path, std::ios::binary);
        REQUIRE(output);
        output.seekp(static_cast<std::streamoff>(beacon::max_json_bytes));
        output.put('\n');
        REQUIRE(output);
    }
    const auto stats = beacon::read_modern_stats(stats_path);
    REQUIRE_FALSE(stats);
    REQUIRE(stats.error().code == beacon::ErrorCode::SecurityLimit);
}

TEST_CASE("Modern stats expose a non-authoritative estimated item count") {
    const beacon::test::TemporaryDirectory temporary;
    const auto& root = temporary.path;
    const auto stats_path = root / "player.json";
    write_text(
        stats_path,
        R"({"DataVersion":4189,"stats":{"minecraft:custom":{"minecraft:play_time":1},"minecraft:picked_up":{"minecraft:diamond":10},"minecraft:crafted":{"minecraft:diamond":2},"minecraft:dropped":{"minecraft:diamond":3},"minecraft:used":{"minecraft:diamond":1}}})");
    const auto stats = beacon::read_modern_stats(stats_path);
    REQUIRE(stats);
    REQUIRE(stats->facts.at("stat/minecraft:estimated/minecraft:diamond") == 8);
    REQUIRE(stats->facts.at("stat/minecraft:picked_up/minecraft:diamond") == 10);
}

TEST_CASE("estimated TNT count subtracts placed TNT") {
    const beacon::test::TemporaryDirectory temporary;
    const auto& root = temporary.path;
    const auto stats_path = root / "player.json";
    write_text(
        stats_path,
        R"({"DataVersion":4189,"stats":{"minecraft:custom":{"minecraft:play_time":1},"minecraft:picked_up":{"minecraft:tnt":9},"minecraft:used":{"minecraft:tnt":3}}})");
    const auto stats = beacon::read_modern_stats(stats_path);
    REQUIRE(stats);
    REQUIRE(stats->facts.at("stat/minecraft:estimated/minecraft:tnt") == 6);
}

TEST_CASE("estimated item counts include crafted and removed-only items without scanning unrelated stats") {
    const beacon::test::TemporaryDirectory temporary;
    const auto& root = temporary.path;
    const auto stats_path = root / "player.json";
    write_text(stats_path, R"({"DataVersion":4189,"stats":{
        "minecraft:custom":{"minecraft:play_time":1},
        "minecraft:crafted":{"minecraft:diamond":2},
        "minecraft:dropped":{"minecraft:stone":3},
        "minecraft:used":{"minecraft:tnt":1},
        "minecraft:mined":{"minecraft:coal":4}
    }})");
    const auto stats = beacon::read_modern_stats(stats_path);
    REQUIRE(stats);
    REQUIRE(stats->facts.at("stat/minecraft:estimated/minecraft:diamond") == 2);
    REQUIRE(stats->facts.at("stat/minecraft:estimated/minecraft:stone") == 0);
    REQUIRE(stats->facts.at("stat/minecraft:estimated/minecraft:tnt") == 0);
    REQUIRE_FALSE(stats->facts.contains("stat/minecraft:estimated/minecraft:coal"));
}

TEST_CASE("Modern runtime tolerates a missing advancements file") {
    const beacon::test::TemporaryDirectory temporary;
    const auto& root = temporary.path;
    const auto world = root / "world";
    std::filesystem::create_directories(world / "stats");
    std::filesystem::copy_file(fixture("world/stats/player-one.json"), world / "stats/player-one.json");
    const auto compiled_value = beacon::compile_template_json(
        R"({"name_key":"template.modern_test","minecraft":{"min_version":3463,"max_version":4903},"goals":[{"id":"complete","fact":"stat/minecraft:mined/minecraft:stone","target":3}],"completion_rule":"complete"})");
    REQUIRE(compiled_value);
    auto compiled = std::make_shared<const beacon::CompiledTemplate>(*compiled_value);
    auto selected = selection(world, compiled);

    beacon::Runtime runtime;
    REQUIRE(runtime.select(std::move(selected)));
    const auto processed = runtime.process_next(std::chrono::seconds(2));
    const auto detail =
        processed.has_value() ? std::string{} : processed.error().message + ": " + processed.error().context;
    INFO(detail);
    REQUIRE(processed);
    REQUIRE(runtime.state()->snapshot.results[compiled->completion_node].done);
}

TEST_CASE("missing stats never become an empty facts snapshot") {
    const beacon::test::TemporaryDirectory temporary;
    const auto& root = temporary.path;
    const beacon::WorldLocation world{{"Fixture world", "world-a"}, root / "world"};
    const beacon::VersionProfile profile{.data_version = 4189, .layout = beacon::SaveLayout::WorldRoot};

    const auto facts = beacon::read_minecraft_facts(world, {"player-one"}, profile);
    REQUIRE_FALSE(facts);
    REQUIRE(facts.error().code == beacon::ErrorCode::Io);
}

TEST_CASE("configured Minecraft directory falls back to an empty template and recovers") {
    const beacon::test::TemporaryDirectory temporary;
    const auto& root = temporary.path;
    const auto game_root = root / ".minecraft";
    const auto compiled_value = beacon::compile_template_json(
        R"({"name_key":"template.modern_test","minecraft":{"min_version":3463,"max_version":4903},"goals":[{"id":"complete","fact":"stat/minecraft:mined/minecraft:stone","target":3}],"completion_rule":"complete"})");
    REQUIRE(compiled_value);
    auto compiled = std::make_shared<const beacon::CompiledTemplate>(*compiled_value);
    const auto localization = beacon::compile_localization_json(R"({"template.modern_test":"Modern test"})");
    const auto layout = beacon::compile_layout_json(R"({"main":[],"overlay":[]})", *compiled);
    REQUIRE(localization);
    REQUIRE(layout);

    beacon::Settings settings;
    settings.game_root = game_root;
    beacon::Runtime runtime;
    REQUIRE(runtime.configure(settings.game_root, compiled, std::make_shared<const beacon::Localization>(*localization),
                              std::make_shared<const beacon::Layout>(*layout)));
    REQUIRE(runtime.state());
    REQUIRE_FALSE(runtime.state()->has_data());
    REQUIRE_FALSE(runtime.process_next(std::chrono::seconds(2)));
    REQUIRE(runtime.state()->error);
    REQUIRE(runtime.state()->error->code == beacon::ErrorCode::NoSave);

    const auto world = game_root / "saves/world";
    std::filesystem::create_directories(world / "stats");
    std::filesystem::copy_file(fixture("world/stats/player-one.json"), world / "stats/player-one.json");
    REQUIRE(runtime.poll_files());
    process_until_published(runtime);
    REQUIRE(runtime.state()->has_data());
    REQUIRE_FALSE(runtime.state()->error);
}

TEST_CASE("active source ignores level.dat and uses player progress") {
    const beacon::test::TemporaryDirectory temporary;
    const auto& root = temporary.path;
    const auto game_root = root / ".minecraft";
    const auto old_world = game_root / "saves/old";
    const auto new_world = game_root / "saves/new";
    std::filesystem::create_directories(old_world / "stats");
    std::filesystem::create_directories(new_world / "stats");
    std::filesystem::copy_file(fixture("world/stats/player-one.json"), old_world / "stats/player-one.json");
    std::ofstream(new_world / "level.dat") << "x";
    const auto now = std::filesystem::file_time_type::clock::now();
    std::filesystem::last_write_time(old_world / "stats/player-one.json", now - std::chrono::hours(1));
    std::filesystem::last_write_time(new_world / "level.dat", now);

    const auto source = beacon::detect_active_source(game_root);
    REQUIRE(source);
    REQUIRE(source->world.path == old_world);
    REQUIRE(source->player.uuid == "player-one");
}

TEST_CASE("active source atomically selects the newest world player and layout") {
    const beacon::test::TemporaryDirectory temporary;
    const auto& root = temporary.path;
    const auto game_root = root / ".minecraft";
    const auto world_path = game_root / "saves/world";
    const auto players_path = world_path / "players";
    std::filesystem::create_directories(world_path / "stats");
    std::filesystem::create_directories(world_path / "advancements");
    std::filesystem::create_directories(players_path / "stats");
    std::filesystem::create_directories(players_path / "advancements");
    write_text(world_path / "stats/player-one.json", read_text(fixture("world/stats/player-one.json")));
    write_text(world_path / "advancements/player-one.json", read_text(fixture("world/advancements/player-one.json")));
    write_text(players_path / "stats/player-one.json", read_text(fixture("world/stats/player-one.json")));
    write_text(players_path / "advancements/player-one.json", read_text(fixture("world/advancements/player-one.json")));
    const auto now = std::filesystem::file_time_type::clock::now();
    std::filesystem::last_write_time(players_path / "stats/player-one.json", now - std::chrono::hours(1));
    std::filesystem::last_write_time(players_path / "advancements/player-one.json", now - std::chrono::hours(1));
    std::filesystem::last_write_time(world_path / "stats/player-one.json", now);
    std::filesystem::last_write_time(world_path / "advancements/player-one.json", now);

    const auto source = beacon::detect_active_source(game_root);
    REQUIRE(source);
    REQUIRE(source->world.path == world_path);
    REQUIRE(source->player.uuid == "player-one");
    REQUIRE(source->profile.layout == beacon::SaveLayout::WorldRoot);
    REQUIRE(source->profile.data_version == 4189);

    const auto facts = beacon::read_minecraft_facts(source->world, source->player, source->profile);
    REQUIRE(facts);
    REQUIRE(facts->at("clock/play_ticks") == 120);
}

TEST_CASE("active source skips newest world when its version is incompatible") {
    const beacon::test::TemporaryDirectory temporary;
    const auto& root = temporary.path;
    const auto game_root = root / ".minecraft";
    const auto old_world = game_root / "saves/compatible";
    const auto new_world = game_root / "saves/incompatible";
    for (const auto& world : {old_world, new_world}) {
        std::filesystem::create_directories(world / "stats");
        std::filesystem::copy_file(fixture("world/stats/player-one.json"), world / "stats/player-one.json");
    }
    auto incompatible = read_text(new_world / "stats/player-one.json");
    incompatible.replace(incompatible.find("4189"), 4, "9999");
    write_text(new_world / "stats/player-one.json", incompatible);
    const auto now = std::filesystem::file_time_type::clock::now();
    std::filesystem::last_write_time(old_world / "stats/player-one.json", now - std::chrono::hours(1));
    std::filesystem::last_write_time(new_world / "stats/player-one.json", now);

    const auto compiled_value = beacon::compile_template_json(read_text(fixture("template.json")));
    REQUIRE(compiled_value);
    const auto source = beacon::detect_active_source(game_root, nullptr, &*compiled_value);
    REQUIRE(source);
    REQUIRE(source->world.path == old_world);
    REQUIRE(source->profile.data_version == 4189);
}

TEST_CASE("active source falls back when newest stats fail full validation") {
    const beacon::test::TemporaryDirectory temporary;
    const auto& root = temporary.path;
    const auto game_root = root / ".minecraft";
    const auto old_world = game_root / "saves/compatible";
    const auto new_world = game_root / "saves/incomplete";
    std::filesystem::create_directories(old_world / "stats");
    std::filesystem::create_directories(new_world / "stats");
    std::filesystem::copy_file(fixture("world/stats/player-one.json"), old_world / "stats/player-one.json");
    write_text(new_world / "stats/player-one.json", R"({"DataVersion":4189,"stats":{"minecraft:custom":{}}})");
    const auto now = std::filesystem::file_time_type::clock::now();
    std::filesystem::last_write_time(old_world / "stats/player-one.json", now - std::chrono::hours(1));
    std::filesystem::last_write_time(new_world / "stats/player-one.json", now);

    const auto compiled = beacon::compile_template_json(read_text(fixture("template.json")));
    REQUIRE(compiled);
    const auto source = beacon::detect_active_source(game_root, nullptr, &*compiled);
    REQUIRE(source);
    REQUIRE(source->world.path == old_world);
    REQUIRE(source->profile.data_version == 4189);
}

TEST_CASE("active source falls back when the current stats become invalid") {
    const beacon::test::TemporaryDirectory temporary;
    const auto& root = temporary.path;
    const auto game_root = root / ".minecraft";
    const auto old_world = game_root / "saves/old";
    const auto current_world = game_root / "saves/current";
    for (const auto& world : {old_world, current_world}) {
        std::filesystem::create_directories(world / "stats");
        std::filesystem::copy_file(fixture("world/stats/player-one.json"), world / "stats/player-one.json");
    }
    const auto now = std::filesystem::file_time_type::clock::now();
    std::filesystem::last_write_time(old_world / "stats/player-one.json", now - std::chrono::hours(1));
    std::filesystem::last_write_time(current_world / "stats/player-one.json", now);

    const auto compiled = beacon::compile_template_json(read_text(fixture("template.json")));
    REQUIRE(compiled);
    beacon::SourceDiscovery discovery;
    const auto current = discovery.scan(game_root, nullptr, &*compiled);
    REQUIRE(current);
    REQUIRE(current->world.path == current_world);

    write_text(current_world / "stats/player-one.json", "not json");
    const auto fallback = discovery.scan(game_root, &*current, &*compiled);
    REQUIRE(fallback);
    REQUIRE(fallback->world.path == old_world);
}

TEST_CASE("active source skips a candidate with a non-file advancements path") {
    const beacon::test::TemporaryDirectory temporary;
    const auto& root = temporary.path;
    const auto game_root = root / ".minecraft";
    const auto old_world = game_root / "saves/old";
    const auto new_world = game_root / "saves/new";
    for (const auto& world : {old_world, new_world}) {
        std::filesystem::create_directories(world / "stats");
        std::filesystem::copy_file(fixture("world/stats/player-one.json"), world / "stats/player-one.json");
    }
    std::filesystem::create_directories(new_world / "advancements/player-one.json");
    const auto now = std::filesystem::file_time_type::clock::now();
    std::filesystem::last_write_time(old_world / "stats/player-one.json", now - std::chrono::hours(1));
    std::filesystem::last_write_time(new_world / "stats/player-one.json", now);

    const auto compiled = beacon::compile_template_json(read_text(fixture("template.json")));
    REQUIRE(compiled);
    const auto source = beacon::detect_active_source(game_root, nullptr, &*compiled);
    REQUIRE(source);
    REQUIRE(source->world.path == old_world);
}

#if !defined(_WIN32)
TEST_CASE("active source ignores save directories outside the configured game root") {
    const beacon::test::TemporaryDirectory temporary;
    const auto& root = temporary.path;
    const auto game_root = root / ".minecraft";
    const auto outside_world = root / "outside-world";
    std::filesystem::create_directories(game_root / "saves");
    std::filesystem::create_directories(outside_world / "stats");
    std::filesystem::copy_file(fixture("world/stats/player-one.json"), outside_world / "stats/player-one.json");
    std::filesystem::create_symlink(outside_world, game_root / "saves/linked-world");

    const auto source = beacon::detect_active_source(game_root);
    REQUIRE_FALSE(source);
}
#endif

TEST_CASE("active source skips an unrelated malformed stats file") {
    const beacon::test::TemporaryDirectory temporary;
    const auto& root = temporary.path;
    const auto game_root = root / ".minecraft";
    const auto old_world = game_root / "saves/compatible";
    const auto new_world = game_root / "saves/malformed";
    std::filesystem::create_directories(old_world / "stats");
    std::filesystem::create_directories(new_world / "stats");
    std::filesystem::copy_file(fixture("world/stats/player-one.json"), old_world / "stats/player-one.json");
    write_text(new_world / "stats/player-one.json", "not json");
    const auto now = std::filesystem::file_time_type::clock::now();
    std::filesystem::last_write_time(old_world / "stats/player-one.json", now - std::chrono::hours(1));
    std::filesystem::last_write_time(new_world / "stats/player-one.json", now);

    const auto compiled = beacon::compile_template_json(read_text(fixture("template.json")));
    REQUIRE(compiled);
    const auto source = beacon::detect_active_source(game_root, nullptr, &*compiled);
    REQUIRE(source);
    REQUIRE(source->world.path == old_world);
}

TEST_CASE("game directory follows the newest save and switches when another save changes") {
    const beacon::test::TemporaryDirectory temporary;
    const auto& root = temporary.path;
    const auto game_root = root / ".minecraft";
    const auto old_world = game_root / "saves/old";
    const auto new_world = game_root / "saves/new";
    for (const auto& world : {old_world, new_world}) {
        std::filesystem::create_directories(world / "stats");
        std::filesystem::create_directories(world / "advancements");
        std::filesystem::copy_file(fixture("world/stats/player-one.json"), world / "stats/player-one.json");
        std::filesystem::copy_file(fixture("world/advancements/player-one.json"),
                                   world / "advancements/player-one.json");
    }
    const auto now = std::filesystem::file_time_type::clock::now();
    const auto stamp = [&](const auto& world, const auto time) {
        std::filesystem::last_write_time(world / "stats/player-one.json", time);
        std::filesystem::last_write_time(world / "advancements/player-one.json", time);
    };
    stamp(old_world, now - std::chrono::hours(1));
    stamp(new_world, now);

    const auto active = beacon::detect_active_source(game_root);
    REQUIRE(active);
    REQUIRE(active->world.path == new_world);
    REQUIRE(active->player.uuid == "player-one");

    const auto compiled_value = beacon::compile_template_json(read_text(fixture("template.json")));
    REQUIRE(compiled_value);
    auto compiled = std::make_shared<const beacon::CompiledTemplate>(*compiled_value);
    const auto localization = beacon::compile_localization_json(R"({"template.modern_test":"Modern test"})");
    const auto layout = beacon::compile_layout_json(R"({"main":[],"overlay":[]})", *compiled);
    REQUIRE(localization);
    REQUIRE(layout);
    beacon::Settings settings;
    settings.game_root = game_root;
    settings.template_path = fixture("template.json");
    beacon::Persistence persistence(root / "settings");
    REQUIRE(persistence.save_settings(settings));
    const auto loaded = persistence.load_settings();
    REQUIRE(loaded);
    REQUIRE(*loaded);
    REQUIRE((**loaded).game_root == game_root);
    beacon::Runtime runtime;
    REQUIRE(runtime.configure((**loaded).game_root, compiled,
                              std::make_shared<const beacon::Localization>(*localization),
                              std::make_shared<const beacon::Layout>(*layout)));
    process_until_published(runtime);
    REQUIRE(runtime.state()->snapshot.run_epoch == 1);
    REQUIRE(runtime.state()->run.world.display_name == "new");

    const auto current_stats = new_world / "stats/player-one.json";
    const auto current_text = read_text(current_stats);
    const auto current_time = std::filesystem::last_write_time(current_stats);
    write_text(current_stats, std::string(current_text.size(), 'x'));
    std::filesystem::last_write_time(current_stats, current_time);
    REQUIRE(runtime.poll_files());
    REQUIRE_FALSE(runtime.process_next(std::chrono::seconds(2)));
    REQUIRE(runtime.state()->data_status == beacon::DataStatus::Stale);
    REQUIRE(runtime.state()->snapshot.play_ticks == 120);
    write_text(current_stats, current_text);
    std::filesystem::last_write_time(current_stats, current_time);

    stamp(old_world, now + std::chrono::hours(1));
    REQUIRE(runtime.poll_files(true));
    process_until_published(runtime);
    REQUIRE(runtime.state()->run.world.display_name == "old");
    REQUIRE(runtime.state()->snapshot.run_epoch == 2);

    std::filesystem::copy_file(fixture("world/stats/player-one.json"), old_world / "stats/player-two.json");
    std::filesystem::copy_file(fixture("world/advancements/player-one.json"),
                               old_world / "advancements/player-two.json");
    std::filesystem::last_write_time(old_world / "stats/player-two.json", now);
    std::filesystem::last_write_time(old_world / "advancements/player-two.json", now + std::chrono::hours(2));
    REQUIRE(runtime.poll_files(true));
    process_until_published(runtime);
    REQUIRE(runtime.state()->run.player.uuid == "player-two");
    REQUIRE(runtime.state()->run.world.display_name == "old");
    REQUIRE(runtime.state()->snapshot.run_epoch == 3);
}

TEST_CASE("settings persistence recovers from backup") {
    const beacon::test::TemporaryDirectory temporary;
    const auto& root = temporary.path;
    beacon::Persistence persistence(root);
    beacon::Settings settings;
    settings.game_root = root / ".minecraft";
    settings.template_path = fixture("template.json");
    REQUIRE(persistence.save_settings(settings));
    settings.game_root = root / "changed";
    REQUIRE(persistence.save_settings(settings));
    for (const auto invalid : {"{}", "[]", "[1]", "null", "42", "true", "\"invalid\""}) {
        CAPTURE(invalid);
        write_text(root / "config/settings.json", invalid);
        const auto recovered_settings = persistence.load_settings();
        REQUIRE(recovered_settings);
        REQUIRE(*recovered_settings);
        REQUIRE((**recovered_settings).game_root == root / ".minecraft");
        REQUIRE(persistence.save_settings(settings));
    }
    write_text(root / "config/settings.json", "[]");
    write_text(root / "config/settings.json.bak", "[1]");
    const auto invalid = persistence.load_settings();
    REQUIRE_FALSE(invalid);
    REQUIRE(invalid.error().code == beacon::ErrorCode::Validation);
}

TEST_CASE("runtime rebuilds atomically and skips unchanged files") {
    const beacon::test::TemporaryDirectory temporary;
    const auto& root = temporary.path;
    const auto world = root / "saves/world";
    std::filesystem::create_directories(world / "stats");
    std::filesystem::create_directories(world / "advancements");
    std::filesystem::copy_file(fixture("world/stats/player-one.json"), world / "stats/player-one.json");
    std::filesystem::copy_file(fixture("world/advancements/player-one.json"), world / "advancements/player-one.json");
    const auto compiled_value = beacon::compile_template_json(read_text(fixture("template.json")));
    REQUIRE(compiled_value);
    auto compiled = std::make_shared<const beacon::CompiledTemplate>(*compiled_value);

    const auto data_root = root / "state-root";
    beacon::Settings settings;
    settings.game_root = root;
    settings.template_path = fixture("template.json");
    beacon::Persistence persistence(data_root);
    REQUIRE(persistence.save_settings(settings));
    const auto loaded_settings = persistence.load_settings();
    REQUIRE(loaded_settings);
    REQUIRE(*loaded_settings);

    beacon::Runtime runtime;
    const auto localization = beacon::compile_localization_json(R"({"template.modern_test":"Modern test"})");
    const auto layout = beacon::compile_layout_json(R"({"main":[],"overlay":[]})", *compiled);
    REQUIRE(localization);
    REQUIRE(layout);
    REQUIRE(runtime.configure((**loaded_settings).game_root, compiled,
                              std::make_shared<const beacon::Localization>(*localization),
                              std::make_shared<const beacon::Layout>(*layout)));
    REQUIRE(runtime.process_next(std::chrono::seconds(2)));
    REQUIRE(runtime.state());
    REQUIRE(runtime.state()->snapshot.results[compiled->completion_node].done);
    REQUIRE(runtime.state()->snapshot.play_ticks == 120);
    const auto world_key = runtime.state()->run.world.storage_key;
    const auto revision = runtime.state()->snapshot.revision;
    REQUIRE(runtime.poll_files());
    REQUIRE_FALSE(*runtime.process_next(std::chrono::seconds(2)));
    REQUIRE_FALSE(*runtime.process_next(std::chrono::milliseconds(20)));
    REQUIRE(runtime.state()->snapshot.revision == revision);

    write_text(world / "advancements/player-one.json", "{}");
    REQUIRE(runtime.poll_files());
    process_until_published(runtime);
    REQUIRE_FALSE(runtime.state()->snapshot.results[compiled->completion_node].done);
    REQUIRE_FALSE(runtime.state()->snapshot.results[2].done);
    REQUIRE(runtime.select(selection(world, compiled, world_key)));
    process_until_published(runtime);
    REQUIRE_FALSE(runtime.state()->snapshot.results[compiled->completion_node].done);

    write_text(world / "advancements/player-one.json", read_text(fixture("world/advancements/player-one.json")));
    REQUIRE(runtime.poll_files());
    process_until_published(runtime);
    REQUIRE(runtime.state()->snapshot.results[compiled->completion_node].done);

    const auto stable_revision = runtime.state()->snapshot.revision;
    write_text(world / "stats/player-one.json", "{}");
    REQUIRE(runtime.poll_files());
    const auto failed = runtime.process_next(std::chrono::seconds(2));
    REQUIRE_FALSE(failed);
    REQUIRE(runtime.state()->snapshot.revision == stable_revision);
    REQUIRE(runtime.poll_files());
    const auto retried = runtime.process_next(std::chrono::seconds(2));
    REQUIRE_FALSE(retried);
    write_text(world / "stats/player-one.json", read_text(fixture("world/stats/player-one.json")));
    REQUIRE(runtime.poll_files());
    process_until_published(runtime);

    REQUIRE(runtime.select(selection(world, compiled, "world-b")));
    REQUIRE(runtime.process_next(std::chrono::seconds(2)));
    REQUIRE(runtime.state()->snapshot.results[compiled->completion_node].done);

    REQUIRE(runtime.select(selection(world, compiled, world_key)));
    process_until_published(runtime);
    REQUIRE(runtime.state()->snapshot.results[compiled->completion_node].done);

    REQUIRE(runtime.select(selection(world, compiled, "world-b")));
    REQUIRE(runtime.select(selection(world, compiled, world_key)));
    process_until_published(runtime);
    auto later_stats = read_text(fixture("world/stats/player-one.json"));
    later_stats.replace(later_stats.find("120"), 3, "200");
    write_text(world / "stats/player-one.json", later_stats);
    {
        beacon::Runtime restarted;
        auto selected = selection(world, compiled, world_key);
        REQUIRE(restarted.select(std::move(selected)));
        process_until_published(restarted);
        REQUIRE(restarted.state()->snapshot.results[compiled->completion_node].done);
        REQUIRE(restarted.state()->snapshot.play_ticks == 200);
    }
    write_text(world / "stats/player-one.json", read_text(fixture("world/stats/player-one.json")));

    write_text(world / "advancements/player-one.json", "{}");
    {
        beacon::Runtime recreated;
        auto selected = selection(world, compiled, world_key);
        REQUIRE(recreated.select(std::move(selected)));
        process_until_published(recreated);
        REQUIRE_FALSE(recreated.state()->snapshot.results[compiled->completion_node].done);
    }
    write_text(world / "advancements/player-one.json", read_text(fixture("world/advancements/player-one.json")));

    auto changed_text = read_text(fixture("template.json"));
    changed_text.replace(changed_text.find("\"target\": 3"), std::string("\"target\": 3").size(), "\"target\": 4");
    const auto changed_value = beacon::compile_template_json(changed_text);
    REQUIRE(changed_value);
    {
        beacon::Runtime changed_runtime;
        auto changed = std::make_shared<const beacon::CompiledTemplate>(*changed_value);
        REQUIRE(changed_runtime.select(selection(world, changed, world_key)));
        process_until_published(changed_runtime);
        REQUIRE_FALSE(changed_runtime.state()->snapshot.results[changed->completion_node].done);
    }
}

TEST_CASE("runtime keeps manual progress in memory across file refreshes") {
    const beacon::test::TemporaryDirectory temporary;
    const auto& root = temporary.path;
    const auto world = root / "saves/world";
    std::filesystem::create_directories(world / "stats");
    std::filesystem::create_directories(world / "advancements");
    std::filesystem::copy_file(fixture("world/stats/player-one.json"), world / "stats/player-one.json");
    std::filesystem::copy_file(fixture("world/advancements/player-one.json"), world / "advancements/player-one.json");
    const auto compiled_value = beacon::compile_template_json(read_text(fixture("template.json")));
    REQUIRE(compiled_value);
    auto compiled = std::make_shared<const beacon::CompiledTemplate>(*compiled_value);
    beacon::Runtime runtime;
    REQUIRE(runtime.select(selection(world, compiled)));
    process_until_published(runtime);
    const auto counter = static_cast<std::uint32_t>(
        std::ranges::find_if(compiled->graph.nodes,
                             [](const auto& node) {
                                 return node.fact_key == "stat/minecraft:mined/minecraft:stone";
                             }) -
        compiled->graph.nodes.begin());
    REQUIRE(runtime.manual_operation(counter, true));
    REQUIRE(runtime.state()->snapshot.results[counter].value == 4);
    auto stats = read_text(world / "stats/player-one.json");
    const auto position = stats.find("\"minecraft:stone\": 3");
    REQUIRE(position != std::string::npos);
    stats.replace(position + std::string("\"minecraft:stone\": ").size(), 1, "4");
    write_text(world / "stats/player-one.json", stats);
    REQUIRE(runtime.poll_files());
    process_until_published(runtime);
    REQUIRE(runtime.state()->snapshot.results[counter].value == 5);
    REQUIRE(runtime.select(selection(world, compiled)));
    REQUIRE(runtime.state());
    REQUIRE_FALSE(runtime.state()->has_data());
    REQUIRE(runtime.state()->snapshot.play_ticks == 0);
    const auto unavailable = runtime.manual_operation(counter, true);
    REQUIRE_FALSE(unavailable);
    REQUIRE(unavailable.error().code == beacon::ErrorCode::Validation);
    process_until_published(runtime);
    REQUIRE(runtime.state()->snapshot.results[counter].value == 4);
    REQUIRE(runtime.select(selection(root / "missing-world", compiled, "world-b")));
    const auto failed = runtime.process_next(std::chrono::seconds(2));
    REQUIRE_FALSE(failed);
    REQUIRE_FALSE(runtime.state()->has_data());
    REQUIRE(runtime.state()->snapshot.play_ticks == 0);
}

TEST_CASE("runtime matches templates by exact DataVersion") {
    const beacon::test::TemporaryDirectory temporary;
    const auto& root = temporary.path;
    const auto world = root / "world";
    std::filesystem::create_directories(world / "stats");
    std::filesystem::create_directories(world / "advancements");
    std::filesystem::copy_file(fixture("world/stats/player-one.json"), world / "stats/player-one.json");
    std::filesystem::copy_file(fixture("world/advancements/player-one.json"), world / "advancements/player-one.json");

    auto text = read_text(fixture("template.json"));
    text.replace(text.find("3463"), 4, "4190");
    const auto compiled_value = beacon::compile_template_json(text);
    REQUIRE(compiled_value);
    auto compiled = std::make_shared<const beacon::CompiledTemplate>(*compiled_value);
    auto selected = selection(world, compiled);

    beacon::Runtime runtime;
    REQUIRE(runtime.select(selected));
    const auto mismatch = runtime.process_next(std::chrono::seconds(2));
    REQUIRE_FALSE(mismatch);
    REQUIRE(mismatch.error().code == beacon::ErrorCode::UnsupportedVersion);

    auto unsafe = selection(world, compiled);
    unsafe.player.uuid = "../escape";
    REQUIRE_FALSE(runtime.select(std::move(unsafe)));
}

TEST_CASE("presentation changes preserve manual progress including an in-flight rebuild") {
    const beacon::test::TemporaryDirectory temporary;
    const auto& root = temporary.path;
    const auto world = root / "saves/world";
    std::filesystem::create_directories(world / "stats");
    const auto stats_path = world / "stats/player-one.json";
    std::filesystem::copy_file(fixture("world/stats/player-one.json"), stats_path);
    const auto parsed = beacon::compile_template_json(read_text(fixture("template.json")));
    REQUIRE(parsed);
    auto compiled = std::make_shared<const beacon::CompiledTemplate>(*parsed);
    auto run = selection(world, compiled);
    beacon::Runtime runtime;
    REQUIRE(runtime.select(run));
    process_until_published(runtime);
    const auto counter = static_cast<std::uint32_t>(
        std::ranges::find_if(compiled->graph.nodes,
                             [](const auto& node) {
                                 return node.fact_key == "stat/minecraft:mined/minecraft:stone";
                             }) -
        compiled->graph.nodes.begin());
    REQUIRE(runtime.manual_operation(counter, true));
    const auto before = runtime.state();
    auto stats = read_text(stats_path);
    const auto position = stats.find("\"minecraft:stone\": 3");
    REQUIRE(position != std::string::npos);
    stats.replace(position, std::string("\"minecraft:stone\": 3").size(), "\"minecraft:stone\": 12");
    write_text(stats_path, stats);
    REQUIRE(runtime.poll_files());
    REQUIRE_FALSE(runtime.poll_files());  // Only one scan is outstanding.
    auto translated = std::make_shared<beacon::Localization>();
    translated->defaults["template.modern_test"] = "Translated";
    auto presented = std::make_shared<beacon::CompiledTemplate>(*compiled);
    presented->template_name_key = "new.presentation";
    REQUIRE(runtime.configure({}, presented, translated, run.layout));
    REQUIRE(runtime.state()->snapshot.run_epoch == before->snapshot.run_epoch);
    REQUIRE(runtime.state()->snapshot.revision == before->snapshot.revision);
    REQUIRE(runtime.state()->snapshot.results == before->snapshot.results);
    REQUIRE(runtime.state()->snapshot.play_ticks == before->snapshot.play_ticks);
    REQUIRE(runtime.state()->localization == translated);
    process_until_published(runtime);
    REQUIRE(runtime.state()->compiled == presented);
    REQUIRE(runtime.state()->localization == translated);
    REQUIRE(runtime.state()->snapshot.results[counter].value == 13);
    REQUIRE(runtime.state()->snapshot.run_epoch == before->snapshot.run_epoch);
}

TEST_CASE("editing a copied template starts a new run instead of reusing stale rule identity") {
    const auto parsed = beacon::compile_template_json(read_text(fixture("template.json")));
    REQUIRE(parsed);
    auto compiled = std::make_shared<const beacon::CompiledTemplate>(*parsed);
    auto run = selection(fixture("world"), compiled);
    beacon::Runtime runtime;
    REQUIRE(runtime.select(run));
    process_until_published(runtime);
    REQUIRE(runtime.manual_operation(1, true));
    const auto before = runtime.state();
    auto changed = std::make_shared<beacon::CompiledTemplate>(*compiled);
    SECTION("fact target") {
        changed->graph.nodes[1].target = 100;
    }
    SECTION("completion rule") {
        changed->completion_node = 1;
    }
    SECTION("compatibility") {
        ++changed->minecraft.max_data_version;
    }
    REQUIRE(runtime.configure({}, changed, run.localization, run.layout));
    REQUIRE(runtime.state()->snapshot.run_epoch != before->snapshot.run_epoch);
    REQUIRE_FALSE(runtime.state()->has_data());
    REQUIRE(runtime.state()->snapshot.results[1].value == 0);
    REQUIRE(before->snapshot.results[1].value == 4);
}

TEST_CASE("configuration preparation and failed persistence leave the live run untouched") {
    const beacon::test::TemporaryDirectory temporary;
    const auto& root = temporary.path;
    const auto parsed = beacon::compile_template_json(read_text(fixture("template.json")));
    REQUIRE(parsed);
    auto compiled = std::make_shared<const beacon::CompiledTemplate>(*parsed);
    auto run = selection(fixture("world"), compiled);
    beacon::Runtime runtime;
    REQUIRE(runtime.select(run));
    process_until_published(runtime);
    const auto before = runtime.state();
    auto invalid = std::make_shared<beacon::CompiledTemplate>(*compiled);
    invalid->completion_node = static_cast<std::uint32_t>(invalid->graph.nodes.size());
    REQUIRE_FALSE(runtime.configure(root, invalid, run.localization, run.layout));
    REQUIRE(runtime.state() == before);
    auto candidate = beacon::Runtime::prepare_configuration(root, compiled, run.localization, run.layout);
    REQUIRE(candidate);
    REQUIRE(runtime.state() == before);
    // A regular file at config/ makes saving fail consistently, including under privileged test users.
    write_text(root / "config", "blocked");
    beacon::Persistence persistence(root);
    beacon::Settings settings{.game_root = root, .template_path = fixture("template.json")};
    REQUIRE_FALSE(persistence.save_settings(settings));
    REQUIRE(runtime.state() == before);
    REQUIRE_FALSE(runtime.state()->error);
    std::filesystem::remove_all(root / "config");
    REQUIRE(persistence.save_settings(settings));
    runtime.commit_configuration(std::move(*candidate));
    REQUIRE_FALSE(runtime.state()->has_data());
    REQUIRE(runtime.state()->snapshot.run_epoch == before->snapshot.run_epoch + 1);
    REQUIRE_FALSE(runtime.process_next(std::chrono::seconds(2)));
    REQUIRE(runtime.state()->error);
}

TEST_CASE("background discovery discards an older configuration and resets changed rules") {
    const beacon::test::TemporaryDirectory temporary;
    const auto& root = temporary.path;
    const auto world = root / "saves/world";
    std::filesystem::create_directories(world / "stats");
    std::filesystem::copy_file(fixture("world/stats/player-one.json"), world / "stats/player-one.json");
    const auto parsed = beacon::compile_template_json(read_text(fixture("template.json")));
    REQUIRE(parsed);
    auto compiled = std::make_shared<const beacon::CompiledTemplate>(*parsed);
    auto run = selection(world, compiled);
    beacon::Runtime runtime;
    REQUIRE(runtime.configure(root / "missing", compiled, run.localization, run.layout));
    REQUIRE(runtime.configure(root, compiled, run.localization, run.layout));
    process_until_published(runtime);
    REQUIRE(runtime.state()->run.world.display_name == "world");
    REQUIRE_FALSE(runtime.state()->error);
    const auto before = runtime.state();
    auto text = read_text(fixture("template.json"));
    text.replace(text.find("\"target\": 3"), std::string("\"target\": 3").size(), "\"target\": 4");
    const auto changed = beacon::compile_template_json(text);
    REQUIRE(changed);
    REQUIRE(runtime.configure(root, std::make_shared<const beacon::CompiledTemplate>(*changed), run.localization,
                              run.layout));
    REQUIRE_FALSE(runtime.state()->has_data());
    REQUIRE(runtime.state()->snapshot.run_epoch == before->snapshot.run_epoch + 1);
    process_until_published(runtime);
    REQUIRE(runtime.state()->compiled->same_rules(*changed));
    REQUIRE_FALSE(runtime.state()->snapshot.results[changed->completion_node].done);
}
