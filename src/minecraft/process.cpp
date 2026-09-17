#include <beacon/minecraft/process.hpp>
#include <beacon/io/file.hpp>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <string_view>
#include <vector>

#if defined(_WIN32)
    #define NOMINMAX
    #include <windows.h>
    #include <winternl.h>
    #include <shellapi.h>
#elif defined(__APPLE__)
    #include <sys/sysctl.h>
#elif defined(BEACON_HAVE_X11)
    #include <X11/Xatom.h>
    #include <X11/Xlib.h>
    #include <cstdlib>
#endif

namespace beacon {
#if defined(__APPLE__)
int foreground_process_id();
#endif

std::optional<std::filesystem::path> minecraft_game_directory(std::span<const std::string> arguments) {
    // Vanilla, Fabric, Forge and launcher wrappers all pass Minecraft's --gameDir option.
    bool minecraft = false;
    std::optional<std::filesystem::path> directory;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const auto& argument = arguments[i];
        minecraft = minecraft || argument.starts_with("net.minecraft.") || argument.starts_with("net.fabricmc.") ||
                    argument.starts_with("org.quiltmc.") || argument == "cpw.mods.modlauncher.Launcher" ||
                    argument == "cpw.mods.bootstraplauncher.BootstrapLauncher" ||
                    argument == "org.multimc.EntryPoint" || argument == "org.prismlauncher.EntryPoint";
        std::string value;
        if (argument == "--gameDir" && i + 1 < arguments.size())
            value = arguments[++i];
        else if (argument.starts_with("--gameDir="))
            value = argument.substr(10);
        else
            continue;
        if (value.empty() || value.find('\0') != std::string::npos || !valid_utf8(value))
            return std::nullopt;
        auto path = path_from_utf8(value);
        // A relative path belongs to the target process, never to Beacon's working directory.
        if (!path.is_absolute())
            return std::nullopt;
        directory = path.lexically_normal();
    }
    return minecraft ? directory : std::nullopt;
}

std::optional<std::filesystem::path> foreground_minecraft_directory() {
    std::vector<std::string> arguments;
#if defined(_WIN32)
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    if (pid == 0 || pid == GetCurrentProcessId())
        return std::nullopt;
    auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return std::nullopt;
    const auto query = reinterpret_cast<decltype(&NtQueryInformationProcess)>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
    ULONG bytes = 0;
    constexpr auto command_line_information = static_cast<PROCESSINFOCLASS>(60);
    if (query)
        query(process, command_line_information, nullptr, 0, &bytes);
    if (!query || bytes < sizeof(UNICODE_STRING) || bytes > 1024 * 1024) {
        CloseHandle(process);
        return std::nullopt;
    }
    std::vector<unsigned char> buffer(bytes);
    const auto status = query(process, command_line_information, buffer.data(), bytes, &bytes);
    CloseHandle(process);
    if (status < 0)
        return std::nullopt;
    const auto* command = reinterpret_cast<const UNICODE_STRING*>(buffer.data());
    if (!command->Buffer || command->Length == 0)
        return std::nullopt;
    const std::wstring command_line(command->Buffer, command->Length / sizeof(wchar_t));
    int count = 0;
    auto argv = CommandLineToArgvW(command_line.c_str(), &count);
    if (!argv)
        return std::nullopt;
    for (int i = 0; i < count; ++i)
        arguments.push_back(path_to_utf8(std::filesystem::path(argv[i])));
    LocalFree(argv);
#elif defined(__APPLE__)
    const auto pid = foreground_process_id();
    if (pid <= 0)
        return std::nullopt;
    int mib[] = {CTL_KERN, KERN_PROCARGS2, pid};
    std::size_t bytes = 0;
    if (sysctl(mib, 3, nullptr, &bytes, nullptr, 0) != 0 || bytes <= sizeof(int) || bytes > 1024 * 1024)
        return std::nullopt;
    std::vector<char> buffer(bytes);
    if (sysctl(mib, 3, buffer.data(), &bytes, nullptr, 0) != 0)
        return std::nullopt;
    int count = 0;
    std::memcpy(&count, buffer.data(), sizeof(count));
    auto cursor = buffer.data() + sizeof(count);
    const auto end = buffer.data() + bytes;
    cursor = std::find(cursor, end, '\0');  // executable path
    while (cursor != end && *cursor == '\0')
        ++cursor;
    for (int i = 0; i < count && cursor != end; ++i) {
        auto next = std::find(cursor, end, '\0');
        if (next == end)
            return std::nullopt;
        arguments.emplace_back(cursor, next);
        cursor = next + 1;
    }
#elif defined(BEACON_HAVE_X11)
    // Native Wayland deliberately does not expose other applications' focused windows.
    const auto session = std::getenv("XDG_SESSION_TYPE");
    if (session && std::string_view(session) == "wayland")
        return std::nullopt;
    auto* display = XOpenDisplay(nullptr);
    if (!display)
        return std::nullopt;
    XSync(display, False);
    const auto previous_handler = XSetErrorHandler([](Display*, XErrorEvent*) {
        return 0;
    });
    const auto property = [&](Window window, const char* name, Atom type) -> unsigned long {
        Atom actual_type = None;
        int format = 0;
        unsigned long count = 0, remaining = 0;
        unsigned char* data = nullptr;
        const auto atom = XInternAtom(display, name, True);
        if (atom == None)
            return 0;
        const auto result = XGetWindowProperty(display, window, atom, 0, 1, False, type, &actual_type, &format, &count,
                                               &remaining, &data);
        const auto value = result == Success && actual_type == type && format == 32 && count == 1 && data
                               ? *reinterpret_cast<unsigned long*>(data)
                               : 0;
        if (data)
            XFree(data);
        return value;
    };
    const auto window = property(DefaultRootWindow(display), "_NET_ACTIVE_WINDOW", XA_WINDOW);
    const auto pid = window ? property(window, "_NET_WM_PID", XA_CARDINAL) : 0;
    XSync(display, False);
    XSetErrorHandler(previous_handler);
    XCloseDisplay(display);
    if (!pid)
        return std::nullopt;
    std::ifstream input("/proc/" + std::to_string(pid) + "/cmdline", std::ios::binary);
    std::string argument;
    std::size_t bytes = 0;
    while (std::getline(input, argument, '\0')) {
        bytes += argument.size() + 1;
        if (bytes > 1024 * 1024)
            return std::nullopt;
        arguments.push_back(argument);
    }
#else
    return std::nullopt;
#endif
    auto directory = minecraft_game_directory(arguments);
    std::error_code error;
    if (!directory || !std::filesystem::is_directory(*directory / "saves", error) || error)
        return std::nullopt;
    return directory;
}

}  // namespace beacon
