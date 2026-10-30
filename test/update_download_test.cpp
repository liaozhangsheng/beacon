#include <beacon/update/download.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>

TEST_CASE("Update downloads validate new destinations and reuse verified cached artifacts", "[update]") {
    const auto root =
        std::filesystem::canonical(std::filesystem::temp_directory_path()) /
        ("beacon-download-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(root);
    struct Cleanup {
        std::filesystem::path root;
        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }
    } cleanup{root};
    const auto destination = root / "artifact.zip";
    constexpr auto hash = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

    // A missing cache file is not an error; the URL is rejected before any network use.
    auto missing = beacon::update::download_file("http://example.invalid/package", destination, 3, hash);
    REQUIRE_FALSE(missing);
    REQUIRE(missing.error().message.find("HTTPS") != std::string::npos);
    REQUIRE_FALSE(std::filesystem::exists(destination));

    std::ofstream(destination, std::ios::binary) << "abc";
    auto cached = beacon::update::download_file("https://example.invalid/package", destination, 3, hash);
    REQUIRE(cached);
    REQUIRE(*cached == destination);
    REQUIRE(*beacon::update::sha256_file(destination) == hash);
    REQUIRE_FALSE(beacon::update::sha256_file(destination, 2));
}

TEST_CASE("HTTPS URLs reject ambiguous authorities and non HTTPS schemes", "[http]") {
    REQUIRE(beacon::http::is_https_url("https://example.org/release.zip?token=a%2Fb"));
    for (const auto* url :
         {"http://example.org/a", "https://", "https://user@example.org/a", "https://example.org\\@other.org/a",
          "https://example.org/a\nHeader:x", "https://example.org/a#fragment", "https://%65xample.org/a"}) {
        INFO(url);
        REQUIRE_FALSE(beacon::http::is_https_url(url));
    }
    REQUIRE_FALSE(beacon::http::is_https_url(std::string_view("https://good.org\0.bad.org", 25)));
}
