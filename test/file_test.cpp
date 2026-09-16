#include "temporary_directory.hpp"

#include <beacon/app/persistence.hpp>
#include <beacon/io/file.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#if !defined(_WIN32)
    #include <sys/stat.h>
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

TEST_CASE("settings persistence preserves UTF-8 filesystem paths") {
    const beacon::test::TemporaryDirectory directory;
    beacon::Persistence persistence(directory.path);
    beacon::Settings settings;
    settings.game_root = beacon::path_from_utf8("用户/存档");
    settings.template_path = beacon::path_from_utf8("模板/template.json");

    REQUIRE(persistence.save_settings(settings));
    const auto loaded = persistence.load_settings();
    REQUIRE(loaded);
    REQUIRE(*loaded);
    REQUIRE((**loaded).game_root == settings.game_root);
    REQUIRE((**loaded).template_path == settings.template_path);
}

#if !defined(_WIN32)
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
