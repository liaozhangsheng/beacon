#include "temporary_directory.hpp"

#include <beacon/update/engine.hpp>
#include <beacon/update/layout.hpp>
#include <beacon/update/version.hpp>

#include <archive.h>
#include <archive_entry.h>
#include <openssl/evp.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#if !defined(_WIN32)
    #include <unistd.h>
#endif

namespace {

using Entry = std::pair<std::string, std::string>;

void write_text(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    REQUIRE(output.good());
}

std::string sha256(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    REQUIRE(context != nullptr);
    REQUIRE(EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1);
    std::array<char, 4096> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) {
            REQUIRE(EVP_DigestUpdate(context, buffer.data(), static_cast<std::size_t>(count)) == 1);
        }
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int size = 0;
    REQUIRE(EVP_DigestFinal_ex(context, digest.data(), &size) == 1);
    EVP_MD_CTX_free(context);
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (unsigned int i = 0; i != size; ++i) {
        result += hex[digest[i] >> 4];
        result += hex[digest[i] & 0xf];
    }
    return result;
}

std::filesystem::path make_zip(const std::filesystem::path& path, const std::vector<Entry>& entries) {
    archive* raw = archive_write_new();
    REQUIRE(raw != nullptr);
    REQUIRE(archive_write_set_format_zip(raw) == ARCHIVE_OK);
    REQUIRE(archive_write_open_filename(raw, path.string().c_str()) == ARCHIVE_OK);
    for (const auto& [name, data] : entries) {
        archive_entry* entry = archive_entry_new();
        archive_entry_set_pathname(entry, name.c_str());
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0644);
        archive_entry_set_size(entry, static_cast<la_int64_t>(data.size()));
        REQUIRE(archive_write_header(raw, entry) == ARCHIVE_OK);
        REQUIRE(archive_write_data(raw, data.data(), data.size()) == static_cast<la_ssize_t>(data.size()));
        archive_entry_free(entry);
    }
    REQUIRE(archive_write_close(raw) == ARCHIVE_OK);
    REQUIRE(archive_write_free(raw) == ARCHIVE_OK);
    return path;
}

beacon::update::Component component(const std::string& id, const std::filesystem::path& archive) {
    return {.url = "https://updates.example.invalid/" + id + ".zip",
            .sha256 = sha256(archive),
            .size = std::filesystem::file_size(archive)};
}

// A component that is never downloaded because the plan reuses it.
beacon::update::Component unchanged(const std::string& id, const char digit) {
    return {.url = "https://updates.example.invalid/" + id + ".zip", .sha256 = std::string(64, digit), .size = 1};
}

beacon::update::Release release(const std::string& version, const beacon::update::Component& runtime,
                                const beacon::update::Component& assets) {
    return {.version = version, .platform = "test-x64", .runtime = runtime, .assets = assets};
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}

std::string current(const std::filesystem::path& root) {
    return read_text(root / ".beacon-current");
}

void write_metadata(const std::filesystem::path& root, const std::string& version) {
    write_text(root / "versions" / version / ".beacon-version.json",
               R"({"version":")" + version + R"(","platform":"test-x64","components":{"runtime":")" +
                   std::string(64, '1') + R"(","assets":")" + std::string(64, 'a') + R"("}})");
}

// Installs version 1.0.0 as a full package would.
void initialize(const std::filesystem::path& root) {
    const auto program = root / "versions/1.0.0";
    write_text(program / beacon::update::layout::program_name, "old");
    write_text(program / "libold.so", "old-library");
    write_text(program / "plugins/codec.so", "old-plugin");
    write_text(program / "assets/old.txt", "old-asset");
    write_metadata(root, "1.0.0");
    write_text(root / ".beacon-current", "1.0.0\n");
    write_text(root / "templates/user/template.json", "user-template");
    write_text(root / "config/settings.json", "user-config");
}

std::string describe(const ylt::expected<void, beacon::Error>& result) {
    return result ? std::string{} : result.error().message + " [" + result.error().context + "]";
}

void require_unchanged(const std::filesystem::path& root) {
    REQUIRE(current(root) == "1.0.0\n");
    REQUIRE(read_text(root / "versions/1.0.0" / beacon::update::layout::program_name) == "old");
    REQUIRE_FALSE(std::filesystem::exists(root / "versions/.staging"));
    REQUIRE_FALSE(std::filesystem::exists(root / "versions/1.1.0"));
}

}  // namespace

TEST_CASE("shared update versions validate components and compare numerically") {
    using beacon::update::parse_version;
    REQUIRE(parse_version("0.0.0"));
    const auto maximum = parse_version("18446744073709551615.18446744073709551615.18446744073709551615");
    REQUIRE(maximum);
    REQUIRE((*maximum)[0] == UINT64_MAX);
    for (const auto invalid :
         {"", "1.2", "1.2.3.4", ".1.2", "1..2", "1.2.", "01.2.3", "1.02.3", "1.2.03", "+1.2.3", "-1.2.3", " 1.2.3",
          "1.2.3\n", "1.2.a", "18446744073709551616.0.0", "0.18446744073709551616.0", "0.0.18446744073709551616"}) {
        INFO(invalid);
        REQUIRE_FALSE(parse_version(invalid));
    }
    REQUIRE(*parse_version("2.0.0") > *parse_version("1.99.99"));
    REQUIRE(*parse_version("1.10.0") > *parse_version("1.9.99"));
    REQUIRE(*parse_version("1.2.10") > *parse_version("1.2.9"));
}

TEST_CASE("release manifests have exact fields") {
    const auto valid = R"json({
        "version":"1.2.3","platform":"test-x64",
        "components":{
          "runtime":{"url":"https://example.invalid/r.zip",
                     "sha256":"0123456789012345678901234567890123456789012345678901234567890123","size":12},
          "assets":{"url":"https://example.invalid/a.zip",
                    "sha256":"abcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcd","size":12}
        }
    })json";
    REQUIRE(beacon::update::read_release(valid));
    std::string extra = valid;
    extra.replace(extra.find("\"version\""), 0, "\"channel\":\"beta\",");
    REQUIRE_FALSE(beacon::update::read_release(extra));
    REQUIRE_FALSE(beacon::update::read_release(R"({"version":"1.2.3"})"));
    REQUIRE_FALSE(beacon::update::read_release(R"({"version":"1.2","platform":"x","components":{}})"));
    REQUIRE_FALSE(beacon::update::read_release(R"({"version":"1.2.3","platform":"x","extra":1,"components":{}})"));
}

TEST_CASE("a runtime update installs a new version beside the current one") {
    beacon::test::TemporaryDirectory directory;
    const auto& root = directory.path;
    initialize(root);
    const auto archive = make_zip(root / "runtime.zip", {{std::string(beacon::update::layout::program_name), "new"},
                                                         {"libnew.so", "new-library"}});
    const auto target = release("1.1.0", component("runtime-2", archive), unchanged("assets-1", 'a'));
    beacon::update::Engine engine(root);

    const auto plan = engine.plan(target);
    REQUIRE(plan);
    REQUIRE(plan->runtime);
    REQUIRE_FALSE(plan->assets);

    std::filesystem::path checked;
    const auto applied = engine.apply(target, archive, std::nullopt, [&](const std::filesystem::path& program) {
        checked = program;
        return read_text(program / beacon::update::layout::program_name) == "new" &&
               read_text(program / "assets/old.txt") == "old-asset";
    });
    INFO(describe(applied));
    REQUIRE(applied);
    REQUIRE(checked.filename() == ".staging");
    REQUIRE(current(root) == "1.1.0\n");
    const auto program = root / "versions/1.1.0";
    REQUIRE(read_text(program / beacon::update::layout::program_name) == "new");
    REQUIRE(std::filesystem::exists(program / "libnew.so"));
    REQUIRE_FALSE(std::filesystem::exists(program / "libold.so"));
    REQUIRE_FALSE(std::filesystem::exists(program / "plugins"));
    REQUIRE(read_text(program / "assets/old.txt") == "old-asset");
    REQUIRE(read_text(program / ".beacon-version.json").find(sha256(archive)) != std::string::npos);
    // The previous version stays intact for rollback; user data is untouched.
    REQUIRE(read_text(root / "versions/1.0.0" / beacon::update::layout::program_name) == "old");
    REQUIRE(read_text(root / "templates/user/template.json") == "user-template");
    REQUIRE(read_text(root / "config/settings.json") == "user-config");
    REQUIRE_FALSE(std::filesystem::exists(root / "versions/.staging"));
    REQUIRE(engine.current_version() == std::string("1.1.0"));
}

TEST_CASE("an assets update copies the unchanged runtime including subdirectories") {
    beacon::test::TemporaryDirectory directory;
    const auto& root = directory.path;
    initialize(root);
    const auto archive = make_zip(root / "assets.zip", {{"assets/Icon.png", "new-icon"}});
    const auto target = release("1.1.0", unchanged("runtime-1", '1'), component("assets-2", archive));
    const auto applied = beacon::update::Engine(root).apply(target, std::nullopt, archive);
    INFO(describe(applied));
    REQUIRE(applied);
    const auto program = root / "versions/1.1.0";
    REQUIRE(read_text(program / beacon::update::layout::program_name) == "old");
    REQUIRE(read_text(program / "plugins/codec.so") == "old-plugin");
    REQUIRE(read_text(program / "assets/Icon.png") == "new-icon");
    REQUIRE_FALSE(std::filesystem::exists(program / "assets/old.txt"));
}

TEST_CASE("a version-only update needs no archives") {
    beacon::test::TemporaryDirectory directory;
    const auto& root = directory.path;
    initialize(root);
    const auto target = release("1.1.0", unchanged("runtime-1", '1'), unchanged("assets-1", 'a'));
    beacon::update::Engine engine(root);
    const auto plan = engine.plan(target);
    REQUIRE(plan);
    REQUIRE_FALSE(plan->runtime);
    REQUIRE_FALSE(plan->assets);
    REQUIRE(plan->available());
    REQUIRE(engine.apply(target));
    REQUIRE(current(root) == "1.1.0\n");
    REQUIRE(read_text(root / "versions/1.1.0/assets/old.txt") == "old-asset");

    const auto same = engine.plan(target);
    REQUIRE(same);
    REQUIRE_FALSE(same->available());
}

#if !defined(_WIN32)
TEST_CASE("a pointer flush failure keeps the version it already selected") {
    if (::geteuid() == 0) {
        SKIP("directory read permissions do not restrict root");
    }
    beacon::test::TemporaryDirectory directory;
    const auto& root = directory.path;
    initialize(root);
    const auto target = release("1.1.0", unchanged("runtime-1", '1'), unchanged("assets-1", 'a'));
    // Renaming the pointer needs write/execute permission, but opening the
    // root directory for its subsequent flush needs read permission too.
    std::filesystem::permissions(root, std::filesystem::perms::owner_write | std::filesystem::perms::owner_exec);
    const auto applied = beacon::update::Engine(root).apply(target);
    std::filesystem::permissions(root, std::filesystem::perms::owner_all);
    REQUIRE_FALSE(applied);
    REQUIRE(current(root) == "1.1.0\n");
    REQUIRE(std::filesystem::is_regular_file(root / "versions/1.1.0" / beacon::update::layout::program_name));
    REQUIRE(std::filesystem::is_regular_file(root / "versions/1.0.0" / beacon::update::layout::program_name));
    REQUIRE(*beacon::update::Engine(root).current_version() == "1.1.0");
}
#endif

TEST_CASE("invalid releases and failed checks never change the selected version") {
    beacon::test::TemporaryDirectory directory;
    const auto& root = directory.path;
    initialize(root);
    beacon::update::Engine engine(root);
    const auto assets = unchanged("assets-1", 'a');
    const auto program = std::string(beacon::update::layout::program_name);

    const auto good = make_zip(root / "good.zip", {{program, "new"}});
    auto wrong_hash = component("runtime-2", good);
    wrong_hash.sha256[0] = wrong_hash.sha256[0] == '0' ? '1' : '0';
    REQUIRE_FALSE(engine.apply(release("1.1.0", wrong_hash, assets), good));
    require_unchanged(root);

    for (const auto& entries : std::vector<std::vector<Entry>>{
             {{program, "new"}, {"../escape", "bad"}},
             {{program, "new"}, {"assets/sneaky.txt", "bad"}},
             {{program, "new"}, {".beacon-version.json", "{}"}},
             {{program, "new"}, {"c:/escape", "bad"}},
             {{"libonly.so", "no program"}},
         }) {
        const auto archive = make_zip(root / "bad.zip", entries);
        INFO(entries.back().first);
        REQUIRE_FALSE(engine.apply(release("1.1.0", component("runtime-2", archive), assets), archive));
        require_unchanged(root);
    }

    const auto rejected = engine.apply(release("1.1.0", component("runtime-2", good), assets), good, std::nullopt,
                                       [](const std::filesystem::path&) {
                                           return false;
                                       });
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().message == "health check failed");
    require_unchanged(root);

    REQUIRE_FALSE(engine.plan(release("0.9.0", unchanged("runtime-1", '1'), assets)));
    REQUIRE_FALSE(engine.plan(release("1.0.0", component("runtime-2", good), assets)));
    auto other_platform = release("1.1.0", unchanged("runtime-1", '1'), assets);
    other_platform.platform = "other-x64";
    REQUIRE_FALSE(engine.plan(other_platform));
    // Archives must match the plan exactly.
    REQUIRE_FALSE(engine.apply(release("1.1.0", component("runtime-2", good), assets)));
    require_unchanged(root);
}

TEST_CASE("release URLs follow the downloader's HTTPS rules") {
    beacon::test::TemporaryDirectory directory;
    initialize(directory.path);
    auto runtime = unchanged("runtime-1", '1');
    beacon::update::Engine engine(directory.path);
    REQUIRE(engine.plan(release("1.1.0", runtime, unchanged("assets-1", 'a'))));
    for (const auto* url : {"https://example.invalid/a b.zip", "https://user@example.invalid/a.zip",
                            "https://example.invalid/a.zip#part"}) {
        INFO(url);
        runtime.url = url;
        REQUIRE_FALSE(engine.plan(release("1.1.0", runtime, unchanged("assets-1", 'a'))));
    }
}

TEST_CASE("leftovers of an interrupted update are discarded by the next apply") {
    beacon::test::TemporaryDirectory directory;
    const auto& root = directory.path;
    initialize(root);
    write_text(root / "versions/.staging/partial", "staged");
    write_text(root / "versions/1.1.0/stale", "renamed but never selected");
    write_text(root / ".beacon-current.tmp", "1.1");
    write_text(root / ".beacon-current.tmp.123.0", "1.1");
    write_text(root / ".beacon-current.tmp-unrelated", "keep");
    const auto target = release("1.1.0", unchanged("runtime-1", '1'), unchanged("assets-1", 'a'));
    beacon::update::Engine engine(root);
    REQUIRE(engine.plan(target));
    REQUIRE(std::filesystem::exists(root / "versions/.staging/partial"));  // plan() only reads
    REQUIRE(engine.apply(target));
    REQUIRE(current(root) == "1.1.0\n");
    REQUIRE_FALSE(std::filesystem::exists(root / "versions/1.1.0/stale"));
    REQUIRE_FALSE(std::filesystem::exists(root / "versions/.staging"));
    REQUIRE_FALSE(std::filesystem::exists(root / ".beacon-current.tmp"));
    REQUIRE_FALSE(std::filesystem::exists(root / ".beacon-current.tmp.123.0"));
    REQUIRE(read_text(root / ".beacon-current.tmp-unrelated") == "keep");
}

TEST_CASE("older versions are pruned and rollback selects the previous version") {
    beacon::test::TemporaryDirectory directory;
    const auto& root = directory.path;
    initialize(root);
    beacon::update::Engine engine(root);
    const auto runtime = unchanged("runtime-1", '1');
    const auto assets = unchanged("assets-1", 'a');
    REQUIRE_FALSE(engine.rollback());
    REQUIRE(engine.apply(release("1.1.0", runtime, assets)));
    REQUIRE(engine.apply(release("1.2.0", runtime, assets)));
    REQUIRE_FALSE(std::filesystem::exists(root / "versions/1.0.0"));
    REQUIRE(std::filesystem::exists(root / "versions/1.1.0"));

    const auto rolled_back = engine.rollback();
    REQUIRE(rolled_back);
    REQUIRE(*rolled_back == "1.1.0");
    REQUIRE(current(root) == "1.1.0\n");
    REQUIRE(std::filesystem::exists(root / "versions/1.2.0"));
    REQUIRE_FALSE(engine.rollback());

    const auto plan = engine.plan(release("1.2.0", runtime, assets));
    REQUIRE(plan);
    REQUIRE(plan->available());
    REQUIRE(engine.apply(release("1.2.0", runtime, assets)));
    REQUIRE(current(root) == "1.2.0\n");
}

TEST_CASE("a damaged current pointer falls back to the newest usable version") {
    beacon::test::TemporaryDirectory directory;
    const auto& root = directory.path;
    initialize(root);
    write_text(root / "versions/1.1.0" / beacon::update::layout::program_name, "middle");
    write_metadata(root, "1.1.0");
    write_metadata(root, "1.2.0");  // no program: unusable
    beacon::update::Engine engine(root);

    for (const std::string pointer : {"", "1.1", "9.9.9\n", "1.2.0\n"}) {
        write_text(root / ".beacon-current", pointer);
        const auto version = engine.current_version();
        REQUIRE(version);
        REQUIRE(*version == "1.1.0");
    }
    const auto rolled_back = engine.rollback();
    REQUIRE(rolled_back);
    REQUIRE(*rolled_back == "1.0.0");
    REQUIRE(current(root) == "1.0.0\n");

    std::filesystem::remove(root / "versions/1.0.0" / beacon::update::layout::program_name);
    std::filesystem::remove(root / "versions/1.1.0" / beacon::update::layout::program_name);
    REQUIRE_FALSE(engine.current_version());
}

TEST_CASE("rollback skips versions without a program") {
    beacon::test::TemporaryDirectory directory;
    const auto& root = directory.path;
    initialize(root);
    write_metadata(root, "1.1.0");  // newer rollback candidate, but no program
    write_text(root / "versions/1.2.0" / beacon::update::layout::program_name, "current");
    write_metadata(root, "1.2.0");
    write_text(root / ".beacon-current", "1.2.0\n");
    beacon::update::Engine engine(root);
    const auto rolled_back = engine.rollback();
    REQUIRE(rolled_back);
    REQUIRE(*rolled_back == "1.0.0");
    REQUIRE(current(root) == "1.0.0\n");

    write_text(root / ".beacon-current", "1.2.0\n");
    std::filesystem::remove(root / "versions/1.0.0" / beacon::update::layout::program_name);
    REQUIRE_FALSE(engine.rollback());
    REQUIRE(current(root) == "1.2.0\n");
}

TEST_CASE("the installation root is derived from a versioned program directory") {
    beacon::test::TemporaryDirectory directory;
    const auto& root = directory.path;
    initialize(root);
    using beacon::update::layout::installation_root;
    REQUIRE(installation_root(root / "versions/1.0.0/") == root);
    REQUIRE(installation_root(root / "versions/.staging") == root);
    REQUIRE(installation_root(root / "build") == root / "build");
    write_text(root / ".beacon-current", "");
    REQUIRE(installation_root(root / "versions/1.0.0") == root);
    std::filesystem::remove(root / ".beacon-current");
    REQUIRE(installation_root(root / "versions/1.0.0/") == root);
    REQUIRE(
        std::filesystem::is_regular_file(installation_root(root / "versions/1.0.0") / "templates/user/template.json"));
    REQUIRE(installation_root(root / "versions/.staging") == root);
    REQUIRE(installation_root(root / "build/") == root / "build");
}
