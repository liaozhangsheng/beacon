#include <beacon/http/proxy.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <initializer_list>
#include <string>

#if defined(_WIN32)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
    #include <winhttp.h>
#elif defined(__APPLE__)
    #include <SystemConfiguration/SystemConfiguration.h>
#endif

namespace beacon::http {
namespace {

bool equal_ignoring_case(const std::string_view a, const std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const char x, const char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

bool loopback(std::string_view host) {
    if (host.starts_with('[') && host.ends_with(']'))
        host = host.substr(1, host.size() - 2);
    return equal_ignoring_case(host, "localhost") || host.starts_with("127.") || host == "::1";
}

#if defined(_WIN32)
std::string narrow(const wchar_t* text) {
    if (text == nullptr)
        return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1)
        return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
    result.resize(static_cast<std::size_t>(size - 1));
    return result;
}
#endif

// The first of `names` set in the environment, even when empty.
std::optional<std::string> environment(std::initializer_list<const char*> names) {
    for (const auto* name : names) {
#if defined(_WIN32)
        const std::wstring wide(name, name + std::char_traits<char>::length(name));
        if (const wchar_t* value = _wgetenv(wide.c_str()))
            return narrow(value);
#else
        if (const char* value = std::getenv(name))
            return std::string(value);
#endif
    }
    return std::nullopt;
}

#if defined(__APPLE__)

std::string string_value(CFDictionaryRef settings, CFStringRef key) {
    const auto value = static_cast<CFStringRef>(CFDictionaryGetValue(settings, key));
    if (value == nullptr || CFGetTypeID(value) != CFStringGetTypeID())
        return {};
    char buffer[1024] = {};
    return CFStringGetCString(value, buffer, sizeof(buffer), kCFStringEncodingUTF8) ? buffer : "";
}

int number_value(CFDictionaryRef settings, CFStringRef key) {
    const auto value = static_cast<CFNumberRef>(CFDictionaryGetValue(settings, key));
    int number = 0;
    if (value == nullptr || CFGetTypeID(value) != CFNumberGetTypeID() ||
        !CFNumberGetValue(value, kCFNumberIntType, &number))
        return 0;
    return number;
}

std::optional<Proxy> system_proxy(const std::string_view host) {
    const CFDictionaryRef settings = SCDynamicStoreCopyProxies(nullptr);
    if (settings == nullptr)
        return std::nullopt;
    std::optional<Proxy> proxy;
    if (number_value(settings, kSCPropNetProxiesHTTPSEnable) != 0) {
        const int port = number_value(settings, kSCPropNetProxiesHTTPSPort);
        proxy = parse_proxy(string_value(settings, kSCPropNetProxiesHTTPSProxy) + ":" + std::to_string(port));
    }
    if (proxy) {
        std::string bypass;
        if (number_value(settings, kSCPropNetProxiesExcludeSimpleHostnames) != 0)
            bypass = "<local>";
        const auto exceptions =
            static_cast<CFArrayRef>(CFDictionaryGetValue(settings, kSCPropNetProxiesExceptionsList));
        if (exceptions != nullptr && CFGetTypeID(exceptions) == CFArrayGetTypeID()) {
            for (CFIndex index = 0; index < CFArrayGetCount(exceptions); ++index) {
                const auto entry = static_cast<CFStringRef>(CFArrayGetValueAtIndex(exceptions, index));
                char buffer[512] = {};
                if (CFGetTypeID(entry) == CFStringGetTypeID() &&
                    CFStringGetCString(entry, buffer, sizeof(buffer), kCFStringEncodingUTF8))
                    bypass.append(",").append(buffer);
            }
        }
        if (bypasses_proxy(host, bypass))
            proxy.reset();
    }
    CFRelease(settings);
    return proxy;
}

#elif defined(_WIN32)

// "host:port", or per-scheme "http=host:port;https=host:port".
std::optional<Proxy> windows_proxy(const std::string_view list) {
    std::optional<Proxy> fallback;
    std::size_t start = 0;
    while (start <= list.size()) {
        const auto end = std::min(list.find_first_of("; ", start), list.size());
        const auto entry = list.substr(start, end - start);
        start = end + 1;
        const auto equals = entry.find('=');
        if (equals == std::string_view::npos) {
            if (!fallback)
                fallback = parse_proxy(entry);
        } else if (equal_ignoring_case(entry.substr(0, equals), "https")) {
            return parse_proxy(entry.substr(equals + 1));
        }
    }
    return fallback;
}

std::optional<Proxy> system_proxy(const std::string_view host) {
    WINHTTP_CURRENT_USER_IE_PROXY_CONFIG config{};
    if (!WinHttpGetIEProxyConfigForCurrentUser(&config))
        return std::nullopt;
    auto proxy = windows_proxy(narrow(config.lpszProxy));
    if (proxy && bypasses_proxy(host, narrow(config.lpszProxyBypass)))
        proxy.reset();
    for (auto* text : {config.lpszProxy, config.lpszProxyBypass, config.lpszAutoConfigUrl}) {
        if (text != nullptr)
            GlobalFree(text);
    }
    return proxy;
}

#else

std::optional<Proxy> system_proxy(std::string_view) {
    return std::nullopt;
}

#endif

}  // namespace

std::optional<Proxy> parse_proxy(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
        value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        value.remove_suffix(1);
    const auto scheme = value.find("://");
    if (scheme != std::string_view::npos) {
        if (!equal_ignoring_case(value.substr(0, scheme), "http"))
            return std::nullopt;
        value.remove_prefix(scheme + 3);
    }
    if (value.ends_with('/'))
        value.remove_suffix(1);
    if (value.find('@') != std::string_view::npos || value.find('/') != std::string_view::npos)
        return std::nullopt;
    // A URL may leave out the port, which is then HTTP's; a bare "host:port" may not.
    auto colon = value.rfind(':');
    if (colon == std::string_view::npos || value.ends_with(']')) {
        if (scheme == std::string_view::npos)
            return std::nullopt;
        colon = value.size();
    }
    const auto host = value.substr(0, colon);
    const auto port = colon == value.size() ? std::string_view("80") : value.substr(colon + 1);
    int number = 0;
    const auto parsed = std::from_chars(port.data(), port.data() + port.size(), number);
    if (host.empty() || parsed.ec != std::errc{} || parsed.ptr != port.data() + port.size() || number < 1 ||
        number > 65535)
        return std::nullopt;
    // An IPv6 address keeps its brackets in the URL but not for the resolver.
    if (host.starts_with('[') != host.ends_with(']'))
        return std::nullopt;
    const auto bare = host.starts_with('[') ? host.substr(1, host.size() - 2) : host;
    if (bare.empty() || !std::ranges::all_of(bare, [](const char c) {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == ':' || c == '_';
        }))
        return std::nullopt;
    return Proxy{.host = std::string(bare), .port = std::to_string(number)};
}

bool bypasses_proxy(const std::string_view host, const std::string_view bypass) {
    std::size_t start = 0;
    while (start < bypass.size()) {
        const auto end = std::min(bypass.find_first_of(",; ", start), bypass.size());
        auto entry = bypass.substr(start, end - start);
        start = end + 1;
        if (entry.empty())
            continue;
        if (entry == "*")
            return true;
        if (entry == "<local>") {
            if (host.find('.') == std::string_view::npos && host.find(':') == std::string_view::npos)
                return true;
            continue;
        }
        if (entry.starts_with("*."))
            entry.remove_prefix(1);
        if (entry.starts_with('.'))
            entry.remove_prefix(1);
        if (equal_ignoring_case(host, entry) ||
            (host.size() > entry.size() && host[host.size() - entry.size() - 1] == '.' &&
             equal_ignoring_case(host.substr(host.size() - entry.size()), entry)))
            return true;
    }
    return false;
}

ylt::expected<std::optional<Proxy>, std::string> proxy_for(const std::string_view host) {
    if (loopback(host))
        return std::nullopt;
    if (const auto configured = environment({"https_proxy", "HTTPS_PROXY", "all_proxy", "ALL_PROXY"})) {
        if (configured->empty() || bypasses_proxy(host, environment({"no_proxy", "NO_PROXY"}).value_or("")))
            return std::nullopt;
        // Connecting directly would ignore the user's proxy, so a setting Beacon cannot use stops the request.
        if (auto proxy = parse_proxy(*configured))
            return proxy;
        return ylt::unexpected<std::string>{"unsupported proxy setting " + *configured};
    }
    return system_proxy(host);
}

}  // namespace beacon::http
