#include <beacon/io/file.hpp>
#include <beacon/update/download.hpp>
#include <beacon/update/engine.hpp>
#include <beacon/update/layout.hpp>
#include <beacon/update/lock.hpp>

#include <openssl/evp.h>
#include <openssl/pem.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <beacon/update/config.hpp>

#include "common.hpp"

#if defined(_WIN32)
    #define NOMINMAX
    #include <windows.h>
#elif defined(__APPLE__)
    #include <mach-o/dyld.h>
    #include <signal.h>
    #include <spawn.h>
    #include <sys/wait.h>
    #include <unistd.h>
#else
    #include <limits.h>
    #include <signal.h>
    #include <spawn.h>
    #include <sys/wait.h>
    #include <unistd.h>
#endif

#if !defined(_WIN32)
extern char** environ;
#endif

namespace beacon::update {
namespace {

constexpr std::size_t manifest_limit = 4U * 1024U * 1024U;
constexpr std::size_t signature_size = 64;
constexpr auto smoke_timeout = std::chrono::seconds(30);
constexpr auto parent_exit_timeout = std::chrono::seconds(30);

std::string describe(const Error& error) {
    return error.context.empty() ? error.message : error.message + ": " + error.context;
}

void print_usage() {
    std::cerr << "usage: beacon-updater (--check | --apply | --rollback) [options]\n"
                 "  --root PATH                 installation root\n"
                 "  --manifest-url HTTPS_URL    release manifest URL\n"
                 "  --manifest PATH --signature PATH  local signed manifest\n"
                 "  --machine-check             print a stable one-line check result\n"
                 "  --wait-for-parent PID       wait for Beacon to exit before applying\n";
}

struct Options {
    enum class Operation { None, Help, Check, Apply, Rollback } operation = Operation::None;
    std::filesystem::path root;
    std::string manifest_url;
    std::optional<std::filesystem::path> manifest;
    std::optional<std::filesystem::path> signature;
    bool machine_check = false;
    std::optional<std::uint64_t> wait_for_parent;
};

// Runs `beacon --smoke-test` from a prepared version directory; the program
// selects a headless video driver itself.
#if defined(_WIN32)
bool run_smoke_test(const std::filesystem::path& directory) {
    const auto executable = (directory / layout::program_name).wstring();
    std::wstring command_line = L"\"" + executable + L"\" --smoke-test";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command_line.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        directory.wstring().c_str(), &startup, &process)) {
        return false;
    }
    const auto wait_result = WaitForSingleObject(process.hProcess, static_cast<DWORD>(smoke_timeout.count() * 1000));
    if (wait_result != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, INFINITE);
    }
    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return wait_result == WAIT_OBJECT_0 && exit_code == 0;
}
#else
bool run_smoke_test(const std::filesystem::path& directory) {
    auto executable = path_to_utf8(directory / layout::program_name);
    std::string argument = "--smoke-test";
    char* arguments[] = {executable.data(), argument.data(), nullptr};
    pid_t process = 0;
    if (posix_spawn(&process, executable.c_str(), nullptr, nullptr, arguments, environ) != 0)
        return false;
    const auto deadline = std::chrono::steady_clock::now() + smoke_timeout;
    int status_code = 1;
    while (true) {
        const auto result = waitpid(process, &status_code, WNOHANG);
        if (result == process)
            return WIFEXITED(status_code) && WEXITSTATUS(status_code) == 0;
        if (result < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            kill(process, SIGKILL);
            while (waitpid(process, &status_code, 0) < 0 && errno == EINTR) {
            }
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
}
#endif

ylt::expected<std::filesystem::path, Error> self_executable() {
#if defined(_WIN32)
    std::vector<wchar_t> buffer(512);
    while (true) {
        const auto size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (size == 0)
            return failure(ErrorCode::Io, "cannot locate updater executable");
        if (size + 1 < buffer.size())
            return std::filesystem::path(std::wstring(buffer.data(), size));
        buffer.resize(buffer.size() * 2);
    }
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    (void)_NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size + 1, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0)
        return failure(ErrorCode::Io, "cannot locate updater executable");
    return path_from_utf8(std::string_view(buffer.data()));
#else
    std::array<char, PATH_MAX> buffer{};
    const auto size = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (size <= 0)
        return failure(ErrorCode::Io, "cannot locate updater executable");
    return path_from_utf8(std::string_view(buffer.data(), static_cast<std::size_t>(size)));
#endif
}

ylt::expected<std::filesystem::path, Error> default_root() {
    auto executable = self_executable();
    if (!executable)
        return ylt::unexpected<Error>{std::move(executable.error())};
    const auto root = executable->parent_path().parent_path();
    if (root.empty())
        return failure(ErrorCode::Validation, "cannot infer installation root");
    return root;
}

bool valid_root(const std::filesystem::path& root) {
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(root, ec);
    return !ec && status.type() == std::filesystem::file_type::directory;
}

ylt::expected<void, Error> verify_manifest(const std::string_view manifest, const std::string_view signature) {
    if (signature.size() != signature_size)
        return failure(ErrorCode::Validation, "manifest signature must be 64 bytes");
    using BioPtr = std::unique_ptr<BIO, decltype(&BIO_free)>;
    using KeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
    using ContextPtr = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
    BioPtr bio(BIO_new_mem_buf(BEACON_UPDATE_PUBLIC_KEY, static_cast<int>(std::strlen(BEACON_UPDATE_PUBLIC_KEY))),
               BIO_free);
    KeyPtr key(bio ? PEM_read_bio_PUBKEY(bio.get(), nullptr, nullptr, nullptr) : nullptr, EVP_PKEY_free);
    if (!key || EVP_PKEY_base_id(key.get()) != EVP_PKEY_ED25519)
        return failure(ErrorCode::Validation, "embedded release key is not Ed25519");
    ContextPtr context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!context || EVP_DigestVerifyInit(context.get(), nullptr, nullptr, nullptr, key.get()) != 1 ||
        EVP_DigestVerify(context.get(), reinterpret_cast<const unsigned char*>(signature.data()), signature.size(),
                         reinterpret_cast<const unsigned char*>(manifest.data()), manifest.size()) != 1) {
        return failure(ErrorCode::Validation, "manifest signature verification failed");
    }
    return {};
}

ylt::expected<std::pair<std::string, std::string>, Error> load_manifest(const Options& options) {
    if (options.manifest || options.signature) {
        if (!options.manifest || !options.signature)
            return failure(ErrorCode::Validation, "--manifest and --signature must be supplied together");
        auto manifest = read_bounded_file(*options.manifest, "manifest", manifest_limit);
        if (!manifest)
            return ylt::unexpected<Error>{std::move(manifest.error())};
        auto signature = read_bounded_file(*options.signature, "manifest signature", signature_size);
        if (!signature)
            return ylt::unexpected<Error>{std::move(signature.error())};
        return std::pair<std::string, std::string>{std::move(*manifest), std::move(*signature)};
    }
    const auto manifest_url =
        options.manifest_url.empty()
            ? std::string("https://github.com/liaozhangsheng/beacon/releases/latest/download/release-") +
                  BEACON_UPDATE_PLATFORM + ".json"
            : options.manifest_url;
    if (!http::is_https_url(manifest_url))
        return failure(ErrorCode::Validation, "manifest URL must use HTTPS", manifest_url);
    auto manifest = http::get_text(manifest_url, http::Limits{.max_bytes = manifest_limit});
    if (!manifest)
        return ylt::unexpected<Error>{std::move(manifest.error())};
    auto signature = http::get_text(manifest_url + ".sig", http::Limits{.max_bytes = signature_size});
    if (!signature)
        return ylt::unexpected<Error>{std::move(signature.error())};
    return std::pair<std::string, std::string>{std::move(*manifest), std::move(*signature)};
}

ylt::expected<Options, Error> parse_options(const int argc, char** argv) {
    Options options;
    auto root = default_root();
    if (!root)
        return ylt::unexpected<Error>{std::move(root.error())};
    options.root = std::move(*root);
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        const auto value = [&](const std::string_view name) -> ylt::expected<std::string_view, Error> {
            if (index + 1 >= argc)
                return failure(ErrorCode::Validation, std::string(name) + " requires a value");
            ++index;
            return std::string_view(argv[index]);
        };
        if (argument == "--check") {
            if (options.operation != Options::Operation::None)
                return failure(ErrorCode::Validation, "choose exactly one operation");
            options.operation = Options::Operation::Check;
        } else if (argument == "--apply") {
            if (options.operation != Options::Operation::None)
                return failure(ErrorCode::Validation, "choose exactly one operation");
            options.operation = Options::Operation::Apply;
        } else if (argument == "--rollback") {
            if (options.operation != Options::Operation::None)
                return failure(ErrorCode::Validation, "choose exactly one operation");
            options.operation = Options::Operation::Rollback;
        } else if (argument == "--machine-check") {
            options.machine_check = true;
        } else if (argument == "--wait-for-parent") {
            auto next = value(argument);
            if (!next)
                return ylt::unexpected<Error>{std::move(next.error())};
            std::uint64_t pid = 0;
            const auto [end, error] = std::from_chars(next->data(), next->data() + next->size(), pid);
            if (error != std::errc{} || end != next->data() + next->size() || pid == 0)
                return failure(ErrorCode::Validation, "invalid parent process ID");
            options.wait_for_parent = pid;
        } else if (argument == "--root") {
            auto next = value(argument);
            if (!next)
                return ylt::unexpected<Error>{std::move(next.error())};
            options.root = path_from_utf8(*next);
        } else if (argument == "--manifest-url") {
            auto next = value(argument);
            if (!next)
                return ylt::unexpected<Error>{std::move(next.error())};
            options.manifest_url = std::string(*next);
        } else if (argument == "--manifest") {
            auto next = value(argument);
            if (!next)
                return ylt::unexpected<Error>{std::move(next.error())};
            options.manifest = path_from_utf8(*next);
        } else if (argument == "--signature") {
            auto next = value(argument);
            if (!next)
                return ylt::unexpected<Error>{std::move(next.error())};
            options.signature = path_from_utf8(*next);
        } else if (argument == "--help" || argument == "-h") {
            options.operation = Options::Operation::Help;
            return options;
        } else {
            return failure(ErrorCode::Validation, "unknown argument", std::string(argument));
        }
    }
    if (options.operation == Options::Operation::None)
        return failure(ErrorCode::Validation, "an operation is required");
    if (options.operation == Options::Operation::Rollback &&
        (options.manifest || options.signature || !options.manifest_url.empty()))
        return failure(ErrorCode::Validation, "--rollback does not use a manifest");
    if (options.machine_check && options.operation != Options::Operation::Check)
        return failure(ErrorCode::Validation, "--machine-check requires --check");
    if (options.wait_for_parent && options.operation != Options::Operation::Apply)
        return failure(ErrorCode::Validation, "--wait-for-parent requires --apply");
    if ((options.manifest || options.signature) && !options.manifest_url.empty())
        return failure(ErrorCode::Validation, "choose a local manifest or manifest URL");
    return options;
}

ylt::expected<void, Error> wait_for_parent_exit(const std::uint64_t pid) {
    const auto error = [](std::string message) {
        return failure(ErrorCode::Io, std::move(message), "parent process");
    };
#if defined(_WIN32)
    if (pid > std::numeric_limits<DWORD>::max())
        return error("parent process ID is out of range");
    const auto process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (process == nullptr) {
        if (GetLastError() == ERROR_INVALID_PARAMETER)
            return {};
        return error("cannot monitor Beacon process exit");
    }
    const auto result = WaitForSingleObject(process, static_cast<DWORD>(parent_exit_timeout.count() * 1000));
    CloseHandle(process);
    if (result == WAIT_OBJECT_0)
        return {};
    return error(result == WAIT_TIMEOUT ? "timed out waiting for Beacon to exit" : "cannot wait for Beacon to exit");
#else
    if (pid > static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max()))
        return error("parent process ID is out of range");
    const auto parent = static_cast<pid_t>(pid);
    const auto deadline = std::chrono::steady_clock::now() + parent_exit_timeout;
    while (true) {
        // EPERM means the ID now belongs to another user's process, so Beacon,
        // which ran as this user, has exited and its ID was reused.
        if (kill(parent, 0) != 0 && (errno == ESRCH || errno == EPERM))
            return {};
        if (std::chrono::steady_clock::now() >= deadline)
            return error("timed out waiting for Beacon to exit");
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
#endif
}

// Beacon reads and removes this file on its next start.
void write_update_diagnostic(const std::filesystem::path& root, std::string message) {
    constexpr std::size_t limit = 1024;
    const auto path = root / layout::failure_log;
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    if ((!ec && status.type() != std::filesystem::file_type::not_found &&
         status.type() != std::filesystem::file_type::regular) ||
        (ec && ec != std::errc::no_such_file_or_directory))
        return;
    if (message.size() > limit)
        message.resize(limit);
    std::ofstream log(path, std::ios::binary | std::ios::trunc);
    if (log)
        log << message << '\n';
}

std::filesystem::path cached_archive(const std::filesystem::path& root, const Component& component) {
    return root / layout::download_cache / (component.sha256 + ".zip");
}

// Keeps only archives this release still needs, including partial downloads
// from earlier interrupted runs. Cache files are never managed install files.
void prune_download_cache(const std::filesystem::path& root, const std::set<std::filesystem::path>& keep) {
    const auto cache = root / layout::download_cache;
    std::error_code ec;
    if (std::filesystem::symlink_status(cache, ec).type() != std::filesystem::file_type::directory)
        return;
    for (const auto& entry : std::filesystem::directory_iterator(cache, ec)) {
        std::error_code entry_error;
        if (!keep.contains(entry.path()) &&
            entry.symlink_status(entry_error).type() == std::filesystem::file_type::regular)
            std::filesystem::remove(entry.path(), entry_error);
    }
}

void remove_download_cache(const std::filesystem::path& root) {
    const auto cache = root / layout::download_cache;
    std::error_code ec;
    if (std::filesystem::symlink_status(cache, ec).type() == std::filesystem::file_type::directory)
        std::filesystem::remove_all(cache, ec);
}

ylt::expected<void, Error> run(Options options) {
    if (!valid_root(options.root))
        return failure(ErrorCode::Validation, "installation root is not a regular directory",
                       path_to_utf8(options.root));
    std::error_code ec;
    options.root = std::filesystem::canonical(options.root, ec);
    if (ec)
        return failure(ErrorCode::Io, "cannot resolve installation root");
    if (options.wait_for_parent) {
        auto exited = wait_for_parent_exit(*options.wait_for_parent);
        if (!exited)
            return exited;
    }
    const auto mode = options.operation == Options::Operation::Check ? LockMode::Shared : LockMode::Exclusive;
    auto install_lock = InstallLock::acquire(options.root, mode);
    if (!install_lock)
        return ylt::unexpected<Error>{std::move(install_lock.error())};
    Engine engine(options.root);
    if (options.operation == Options::Operation::Rollback) {
        auto version = engine.rollback();
        if (!version)
            return ylt::unexpected<Error>{std::move(version.error())};
        std::cout << "Rolled back. Start Beacon to use version " << *version << ".\n";
        return {};
    }
    if (std::string_view(BEACON_UPDATE_PUBLIC_KEY).empty())
        return failure(
            ErrorCode::Validation,
            "updater has no embedded release public key; configure BEACON_UPDATE_PUBLIC_KEY_FILE when building");
    auto manifest = load_manifest(options);
    if (!manifest)
        return ylt::unexpected<Error>{std::move(manifest.error())};
    auto verified = verify_manifest(manifest->first, manifest->second);
    if (!verified)
        return verified;
    auto release = read_release(manifest->first);
    if (!release)
        return ylt::unexpected<Error>{std::move(release.error())};
    if (release->platform != BEACON_UPDATE_PLATFORM)
        return failure(ErrorCode::Validation, "release platform does not match this updater", release->platform);
    auto plan = engine.plan(*release);
    if (!plan)
        return ylt::unexpected<Error>{std::move(plan.error())};
    if (options.operation == Options::Operation::Check) {
        if (options.machine_check)
            std::cout << (plan->available() ? "BEACON_UPDATE_AVAILABLE\t" : "BEACON_UPDATE_CURRENT\t");
        else
            std::cout << (plan->available() ? "Update available: " : "Beacon is up to date: ");
        std::cout << release->version << '\n';
        return {};
    }

    std::set<std::filesystem::path> needed;
    if (plan->runtime)
        needed.insert(cached_archive(options.root, release->runtime));
    if (plan->assets)
        needed.insert(cached_archive(options.root, release->assets));
    prune_download_cache(options.root, needed);
    const auto download =
        [&](const bool changed,
            const Component& component) -> ylt::expected<std::optional<std::filesystem::path>, Error> {
        if (!changed)
            return std::optional<std::filesystem::path>{};
        auto archive =
            download_file(component.url, cached_archive(options.root, component), component.size, component.sha256);
        if (!archive)
            return ylt::unexpected<Error>{std::move(archive.error())};
        return std::optional<std::filesystem::path>{std::move(*archive)};
    };
    // A failed apply keeps verified archives, so retrying does not download them again.
    auto runtime_archive = download(plan->runtime, release->runtime);
    if (!runtime_archive)
        return ylt::unexpected<Error>{std::move(runtime_archive.error())};
    auto assets_archive = download(plan->assets, release->assets);
    if (!assets_archive)
        return ylt::unexpected<Error>{std::move(assets_archive.error())};
    auto applied = engine.apply(*release, std::move(*runtime_archive), std::move(*assets_archive),
                                [](const std::filesystem::path& directory) {
                                    return run_smoke_test(directory);
                                });
    if (!applied)
        return applied;
    remove_download_cache(options.root);
    std::filesystem::remove(options.root / layout::failure_log, ec);
    std::cout << "Update applied. Start Beacon to use version " << release->version << ".\n";
    return {};
}

}  // namespace
}  // namespace beacon::update

int run_main(int argc, char** argv) {
    using beacon::update::Options;
    auto options = beacon::update::parse_options(argc, argv);
    if (!options) {
        std::cerr << beacon::update::describe(options.error()) << '\n';
        beacon::update::print_usage();
        return 2;
    }
    if (options->operation == Options::Operation::Help) {
        beacon::update::print_usage();
        return 0;
    }
    const bool automatic = options->wait_for_parent.has_value();
    const auto root = options->root;
    beacon::Error error{};
    try {
        auto result = beacon::update::run(std::move(*options));
        if (result)
            return 0;
        error = std::move(result.error());
    } catch (const std::exception& exception) {
        error = beacon::update::make_error(beacon::ErrorCode::Internal, exception.what());
    }
    std::cerr << beacon::update::describe(error) << '\n';
    if (automatic)
        beacon::update::write_update_diagnostic(root, beacon::update::describe(error));
    return 1;
}

#if defined(_WIN32)
std::string utf8_from_wide(const wchar_t* value) {
    if (value == nullptr)
        return {};
    const auto length = static_cast<int>(std::wcslen(value));
    if (length == 0)
        return {};
    const auto bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, length, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0)
        return {};
    std::string result(static_cast<std::size_t>(bytes), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, length, result.data(), bytes, nullptr, nullptr) !=
        bytes)
        return {};
    return result;
}

int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> arguments;
    arguments.reserve(static_cast<std::size_t>(argc));
    for (int index = 0; index < argc; ++index) {
        arguments.push_back(utf8_from_wide(argv[index]));
        if (arguments.back().empty()) {
            std::cerr << "cannot decode command line as UTF-8\n";
            return 2;
        }
    }
    std::vector<char*> pointers;
    pointers.reserve(arguments.size());
    for (auto& argument : arguments)
        pointers.push_back(argument.data());
    return run_main(argc, pointers.data());
}
#else
int main(int argc, char** argv) {
    return run_main(argc, argv);
}
#endif
