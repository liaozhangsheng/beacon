#include "temporary_directory.hpp"

#include <beacon/ui/profile.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>

namespace {

std::filesystem::path cache_skin(const std::filesystem::path& root, const std::string& file, const std::string& png) {
    const auto path = root / "cache/skins" / file;
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << png;
    return path;
}

}  // namespace

TEST_CASE("a fresh cached skin is used without asking Mojang") {
    const beacon::test::TemporaryDirectory temporary;
    const std::string uuid = "069a79f4-44e9-4726-a5be-fca90e38aaf5";
    cache_skin(temporary.path, "another-player_Other.png", "other skin");
    cache_skin(temporary.path, uuid + "_Ignored.txt", "not a skin");
    std::filesystem::create_directory(temporary.path / "cache/skins" / (uuid + "_Directory.png"));
    cache_skin(temporary.path, uuid + "_Notch.png", "skin");

    const auto card = beacon::fetch_player_card(uuid, temporary.path);
    REQUIRE(card);
    CHECK(card->name == "Notch");
    CHECK(card->skin == "skin");
}

TEST_CASE("an old cached skin still shows while it waits to be downloaded again") {
    const beacon::test::TemporaryDirectory temporary;
    const std::string uuid = "069a79f4-44e9-4726-a5be-fca90e38aaf5";
    const auto path = cache_skin(temporary.path, uuid + "_Notch.png", "skin");
    std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now() - std::chrono::hours(48));

    const auto card = beacon::cached_player_card(uuid, temporary.path);
    REQUIRE(card);
    CHECK(card->name == "Notch");
    CHECK(card->skin == "skin");
}

TEST_CASE("offline-mode players get the default card without a request") {
    const beacon::test::TemporaryDirectory temporary;
    // Offline-mode servers derive version 3 UUIDs from the player name.
    const auto card = beacon::fetch_player_card("a0b1c2d3-e4f5-3a6b-8c7d-0e1f2a3b4c5d", temporary.path);
    REQUIRE(card);
    CHECK(card->name == beacon::PlayerCard{}.name);
    CHECK(card->skin.empty());
}
