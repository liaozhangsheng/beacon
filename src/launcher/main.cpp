// Starts the program version selected by <root>/.beacon-current.
//
// The launcher is the stable entry point that shortcuts and users start. It is
// never replaced by an update, so it must stay small and dependency free.

#include <beacon/update/layout.hpp>
#include <beacon/update/version.hpp>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

#if defined(_WIN32)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#else
    #include <climits>
    #include <cstdlib>
    #include <unistd.h>
    #if defined(__APPLE__)
        #include <mach-o/dyld.h>
    #endif
#endif

namespace {

namespace layout = beacon::update::layout;

using beacon::update::parse_version;
using beacon::update::Version;

std::optional<std::string> read_current(const std::filesystem::path& root) {
    std::string text;
#if defined(_WIN32)
    const auto handle = CreateFileW((root / layout::current_pointer).c_str(), GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return std::nullopt;
    char buffer[128];
    DWORD size = 0;
    const bool read = ReadFile(handle, buffer, sizeof(buffer), &size, nullptr) != FALSE;
    CloseHandle(handle);
    if (!read)
        return std::nullopt;
    text.assign(buffer, size);
#else
    auto* file = std::fopen((root / layout::current_pointer).c_str(), "rb");
    if (file == nullptr)
        return std::nullopt;
    char buffer[128];
    text.assign(buffer, std::fread(buffer, 1, sizeof(buffer), file));
    std::fclose(file);
#endif
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
        text.pop_back();
    if (!parse_version(text))
        return std::nullopt;
    return text;
}

// Version directories only appear by renaming a complete, flushed staging
// directory, so one that contains the program can be started.
bool usable(const std::filesystem::path& directory) {
    std::error_code error;
    return std::filesystem::is_regular_file(directory / layout::program_name, error);
}

// Follows .beacon-current. If it is damaged (for example by a power loss) or
// names an unusable directory, the newest complete version is started instead.
std::optional<std::filesystem::path> select_program(const std::filesystem::path& root) {
    const auto versions = root / layout::versions;
    if (const auto current = read_current(root); current && usable(versions / *current))
        return versions / *current / layout::program_name;
    std::optional<std::pair<Version, std::filesystem::path>> newest;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(versions, error)) {
        // u8string never throws on Windows, unlike string() for names outside the ANSI code page.
        const auto name = entry.path().filename().u8string();
        const auto version = parse_version({reinterpret_cast<const char*>(name.data()), name.size()});
        if (version && (!newest || *version > newest->first) && usable(entry.path()))
            newest.emplace(*version, entry.path());
    }
    if (!newest)
        return std::nullopt;
    return newest->second / layout::program_name;
}

}  // namespace

#if defined(_WIN32)

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    const auto fail = [](const wchar_t* message) {
        MessageBoxW(nullptr, message, L"Beacon", MB_OK | MB_ICONERROR);
        return 1;
    };
    std::wstring executable(MAX_PATH, L'\0');
    while (true) {
        const auto size = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        if (size == 0)
            return fail(L"Cannot locate the Beacon installation.");
        if (size < executable.size()) {
            executable.resize(size);
            break;
        }
        executable.resize(executable.size() * 2);
    }
    const auto root = std::filesystem::path(executable).parent_path();
    const auto selected = select_program(root);
    if (!selected)
        return fail(L"The Beacon installation is damaged: no installed version can be started. "
                    L"Reinstall Beacon from a full package.");
    const auto& program = *selected;

    // Forward the original arguments verbatim: skip only argv[0].
    const wchar_t* arguments = GetCommandLineW();
    if (*arguments == L'"') {
        ++arguments;
        while (*arguments != L'\0' && *arguments != L'"')
            ++arguments;
        if (*arguments == L'"')
            ++arguments;
    } else {
        while (*arguments != L'\0' && *arguments != L' ' && *arguments != L'\t')
            ++arguments;
    }
    std::wstring command = L"\"" + program.wstring() + L"\"" + arguments;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup,
                        &process))
        return fail(L"Cannot start the selected Beacon version. Reinstall Beacon from a full package.");
    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hProcess);
    return static_cast<int>(exit_code);
}

#else

namespace {

std::optional<std::filesystem::path> executable_path() {
    #if defined(__APPLE__)
    std::uint32_t size = 0;
    (void)_NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0)
        return std::nullopt;
    buffer.resize(std::char_traits<char>::length(buffer.c_str()));
    #else
    std::string buffer(PATH_MAX, '\0');
    const auto size = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (size <= 0)
        return std::nullopt;
    buffer.resize(static_cast<std::size_t>(size));
    #endif
    // Resolve symlinks so a link placed in a PATH directory still finds the installation.
    char resolved[PATH_MAX];
    if (::realpath(buffer.c_str(), resolved) == nullptr)
        return std::nullopt;
    return std::filesystem::path(resolved);
}

}  // namespace

int main(int, char** argv) {
    const auto executable = executable_path();
    if (!executable) {
        std::fputs("beacon: cannot locate the installation\n", stderr);
        return 127;
    }
    const auto root = executable->parent_path();
    const auto selected = select_program(root);
    if (!selected) {
        std::fputs("beacon: no installed version can be started; reinstall Beacon from a full package\n", stderr);
        return 127;
    }
    const auto program = selected->string();
    // exec keeps the process ID, which the updater uses to wait for Beacon to exit.
    argv[0] = const_cast<char*>(program.c_str());
    ::execv(program.c_str(), argv);
    std::fprintf(stderr, "beacon: cannot start %s; reinstall Beacon from a full package\n", program.c_str());
    return 127;
}

#endif
