#include "temporary_directory.hpp"

#include <beacon/app/persistence.hpp>
#include <beacon/io/file.hpp>
#include <beacon/minecraft/process.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <barrier>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if !defined(_WIN32)
    #include <sys/stat.h>
    #include <unistd.h>
#endif

TEST_CASE("bounded file reads preserve binary data and enforce caller limits") {
    const beacon::test::TemporaryDirectory directory;
    const auto path = directory.path / "input.bin";

    const auto missing = beacon::read_bounded_file(path, "test file", 4);
    REQUIRE_FALSE(missing);
    REQUIRE(missing.error().code == beacon::ErrorCode::Io);

    const std::string bytes("a\0bc", 4);
    {
        std::ofstream output(path, std::ios::binary);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        REQUIRE(output.good());
    }
    const auto exact = beacon::read_bounded_file(path, "test file", bytes.size());
    REQUIRE(exact);
    REQUIRE(*exact == bytes);
    const auto oversized = beacon::read_bounded_file(path, "test file", bytes.size() - 1);
    REQUIRE_FALSE(oversized);
    REQUIRE(oversized.error().code == beacon::ErrorCode::SecurityLimit);

    std::filesystem::resize_file(path, beacon::max_json_bytes + 1);
    const auto default_limit = beacon::read_bounded_file(path, "test file");
    REQUIRE_FALSE(default_limit);
    REQUIRE(default_limit.error().code == beacon::ErrorCode::SecurityLimit);

    std::filesystem::resize_file(path, 0);
    const auto empty = beacon::read_bounded_file(path, "test file", 0);
    REQUIRE(empty);
    REQUIRE(empty->empty());
}

TEST_CASE("filesystem paths round-trip through UTF-8") {
    constexpr std::string_view value = "Beacon/存档";
    REQUIRE(beacon::path_to_utf8(beacon::path_from_utf8(value)) == value);
}

TEST_CASE("settings files may leave out everything but the paths") {
    const beacon::test::TemporaryDirectory directory;
    beacon::Persistence persistence(directory.path);
    std::filesystem::create_directories(directory.path / "config");
    const auto write = [&](const std::string& json) {
        std::ofstream(directory.path / "config/settings.json", std::ios::trunc) << json;
        return persistence.load_settings();
    };
    const auto minimal = write(R"({"game_root":"game","template_path":"template.json","overlay_scroll_speed":90})");
    REQUIRE(minimal);
    REQUIRE(*minimal);
    auto expected = beacon::Settings{.game_root = "game", .template_path = "template.json"};
    expected.overlay_scroll_speed = 90.0F;
    const auto& loaded = **minimal;
    CHECK(loaded.game_root == expected.game_root);
    CHECK(loaded.auto_detect == expected.auto_detect);
    CHECK(loaded.ui_language == expected.ui_language);
    CHECK(loaded.main_window_scale == expected.main_window_scale);
    CHECK(loaded.main_window_background_color == expected.main_window_background_color);
    CHECK(loaded.overlay_window_background_color == expected.overlay_window_background_color);
    CHECK(loaded.overlay_scroll_speed == 90.0F);
    CHECK(loaded.overlay_visible == expected.overlay_visible);

    CHECK_FALSE(write(R"({"template_path":"template.json"})"));
    CHECK_FALSE(write(R"({"game_root":"game"})"));
    CHECK_FALSE(write(R"({"game_root":"game","template_path":"template.json","auto_detect":"yes"})"));
    CHECK_FALSE(write(R"({"game_root":"game","template_path":"template.json","theme":"dark"})"));
    CHECK_FALSE(write(R"({"game_root":"game","template_path":"template.json","overlay_window_scale":9})"));
}

TEST_CASE("settings round-trip validates fields and ranges") {
    const beacon::test::TemporaryDirectory temporary;
    const auto& root = temporary.path;
    beacon::Persistence persistence(root);
    beacon::Settings settings;
    settings.game_root = root / beacon::path_from_utf8("用户/存档");
    settings.template_path = beacon::path_from_utf8("模板/template.json");
    settings.language = "en";
    settings.ui_language = "en";
    settings.auto_detect = false;
    settings.overlay_visible = false;
    settings.overlay_transparent = true;
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
    REQUIRE((**loaded).template_path == settings.template_path);
    REQUIRE((**loaded).language == settings.language);
    REQUIRE((**loaded).ui_language == settings.ui_language);
    REQUIRE((**loaded).auto_detect == settings.auto_detect);
    REQUIRE((**loaded).overlay_visible == settings.overlay_visible);
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

    settings.main_window_scale = 0.25F;
    REQUIRE_FALSE(persistence.save_settings(settings));
}

TEST_CASE("settings persistence recovers from backup") {
    const beacon::test::TemporaryDirectory temporary;
    const auto& root = temporary.path;
    beacon::Persistence persistence(root);
    beacon::Settings settings;
    settings.game_root = root / ".minecraft";
    settings.template_path = root / "template.json";
    REQUIRE(persistence.save_settings(settings));
    settings.game_root = root / "changed";
    REQUIRE(persistence.save_settings(settings));
    for (const auto invalid : {"{}", "[]", "[1]", "null", "42", "true", "\"invalid\""}) {
        CAPTURE(invalid);
        std::ofstream(root / "config/settings.json", std::ios::trunc) << invalid;
        const auto recovered_settings = persistence.load_settings();
        REQUIRE(recovered_settings);
        REQUIRE(*recovered_settings);
        REQUIRE((**recovered_settings).game_root == root / ".minecraft");
        REQUIRE(persistence.save_settings(settings));
    }
    std::ofstream(root / "config/settings.json", std::ios::trunc) << "[]";
    std::ofstream(root / "config/settings.json.bak", std::ios::trunc) << "[1]";
    const auto invalid = persistence.load_settings();
    REQUIRE_FALSE(invalid);
    REQUIRE(invalid.error().code == beacon::ErrorCode::Validation);
}

TEST_CASE("concurrent settings saves preserve complete primary and backup files") {
    const beacon::test::TemporaryDirectory directory;
    beacon::Persistence persistence(directory.path);
    const auto game = directory.path / beacon::path_from_utf8("存档");
    const auto template_path = directory.path / beacon::path_from_utf8("模板/template.json");
    REQUIRE(persistence.save_settings({.game_root = game, .template_path = template_path}));

    std::atomic<int> failures{0};
    std::barrier start(4);
    {
        std::vector<std::jthread> writers;
        for (int index = 0; index < 4; ++index) {
            writers.emplace_back([&, index] {
                beacon::Settings settings{.game_root = game, .template_path = template_path};
                settings.overlay_scroll_speed = static_cast<float>(index);
                start.arrive_and_wait();
                for (int attempt = 0; attempt < 100; ++attempt) {
                    if (!persistence.save_settings(settings))
                        ++failures;
                }
            });
        }
    }
    REQUIRE(failures == 0);
    for (const bool backup : {false, true}) {
        if (backup)
            std::filesystem::remove(directory.path / "config/settings.json");
        const auto loaded = persistence.load_settings();
        REQUIRE(loaded);
        REQUIRE(*loaded);
        REQUIRE((**loaded).game_root == game);
        REQUIRE((**loaded).template_path == template_path);
        REQUIRE((**loaded).overlay_scroll_speed >= 0);
        REQUIRE((**loaded).overlay_scroll_speed <= 3);
    }
    for (const auto& entry : std::filesystem::directory_iterator(directory.path / "config"))
        REQUIRE(entry.path().filename() == "settings.json.bak");
}

TEST_CASE("atomic writes leave unrelated temporary files and clean up after failed replacement") {
    const beacon::test::TemporaryDirectory directory;
    const auto target = directory.path / "state.json";
    auto old_temporary = target;
    old_temporary += ".tmp";
    std::ofstream(old_temporary) << "keep";
    const std::string binary("a\0bc", 4);
    REQUIRE(beacon::write_file_atomically(target, binary));
    REQUIRE(*beacon::read_bounded_file(target, "state") == binary);
    REQUIRE(*beacon::read_bounded_file(old_temporary, "temporary") == "keep");

    std::filesystem::remove(target);
    std::filesystem::create_directory(target);
    std::ofstream(target / "keep") << "original";
    REQUIRE_FALSE(beacon::write_file_atomically(target, "replacement"));
    REQUIRE(*beacon::read_bounded_file(target / "keep", "original") == "original");
    for (const auto& entry : std::filesystem::directory_iterator(directory.path))
        REQUIRE((entry.path() == target || entry.path() == old_temporary));
}

TEST_CASE("atomic writes accept a filename relative to the working directory") {
    const beacon::test::TemporaryDirectory directory;
    const auto previous = std::filesystem::current_path();
    std::filesystem::current_path(directory.path);
    const auto saved = beacon::write_file_atomically("state.json", "relative");
    std::filesystem::current_path(previous);
    REQUIRE(saved);
    REQUIRE(*beacon::read_bounded_file(directory.path / "state.json", "state") == "relative");
}

#if !defined(_WIN32)
TEST_CASE("failed settings recovery saves leave the backup selected") {
    if (::geteuid() == 0) {
        SKIP("directory read permissions do not restrict root");
    }
    const beacon::test::TemporaryDirectory directory;
    beacon::Persistence persistence(directory.path);
    beacon::Settings settings{.game_root = directory.path, .template_path = directory.path / "template.json"};
    REQUIRE(persistence.save_settings(settings));
    const auto config = directory.path / "config";
    const auto target = config / "settings.json";
    const auto backup = config / "settings.json.bak";
    std::filesystem::rename(target, backup);
    REQUIRE(persistence.load_settings());
    settings.main_window_scale = 0.75F;
    std::filesystem::permissions(config, std::filesystem::perms::owner_write | std::filesystem::perms::owner_exec);
    const auto saved = persistence.save_settings(settings);
    std::filesystem::permissions(config, std::filesystem::perms::owner_all);
    REQUIRE_FALSE(saved);
    REQUIRE_FALSE(std::filesystem::exists(target));
    const auto recovered = persistence.load_settings();
    REQUIRE(recovered);
    REQUIRE(*recovered);
    REQUIRE((**recovered).main_window_scale == beacon::Settings{}.main_window_scale);
}

TEST_CASE("bounded file reads reject special files and symbolic links") {
    const beacon::test::TemporaryDirectory directory;
    const auto fifo = directory.path / "input.fifo";
    REQUIRE(::mkfifo(fifo.c_str(), 0600) == 0);
    const auto fifo_result = beacon::read_bounded_file(fifo, "test file");
    REQUIRE_FALSE(fifo_result);
    REQUIRE(fifo_result.error().code == beacon::ErrorCode::Io);

    const auto target = directory.path / "target.json";
    std::ofstream(target) << "{}";
    const auto link = directory.path / "link.json";
    std::filesystem::create_symlink(target, link);
    const auto link_result = beacon::read_bounded_file(link, "test file");
    REQUIRE_FALSE(link_result);
    REQUIRE(link_result.error().code == beacon::ErrorCode::Io);
}
#endif

TEST_CASE("Minecraft process arguments select only an explicit absolute game directory") {
    const auto root = std::filesystem::temp_directory_path() / beacon::path_from_utf8("Minecraft 空 格");
    const auto path = beacon::path_to_utf8(root);
    REQUIRE(beacon::minecraft_game_directory(
                std::vector<std::string>{"java", "net.minecraft.client.main.Main", "--gameDir", path}) == root);
    REQUIRE(beacon::minecraft_game_directory(std::vector<std::string>{
                "java", "net.fabricmc.loader.impl.launch.knot.KnotClient", "--gameDir=" + path}) == root);
    REQUIRE_FALSE(beacon::minecraft_game_directory(std::vector<std::string>{"other", "--gameDir", path}));
    REQUIRE_FALSE(beacon::minecraft_game_directory(
        std::vector<std::string>{"java", "net.minecraft.client.main.Main", "--gameDir"}));
    REQUIRE_FALSE(beacon::minecraft_game_directory(
        std::vector<std::string>{"java", "net.minecraft.client.main.Main", "--gameDir="}));
    REQUIRE_FALSE(beacon::minecraft_game_directory(
        std::vector<std::string>{"java", "net.minecraft.client.main.Main", "--gameDir", "relative"}));
    REQUIRE_FALSE(beacon::minecraft_game_directory(
        std::vector<std::string>{"java", "net.minecraft.client.main.Main", "--gameDir", std::string("/tmp/a\0b", 8)}));
}
