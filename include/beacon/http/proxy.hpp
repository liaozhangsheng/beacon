#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <ylt/util/expected.hpp>

namespace beacon::http {

// An HTTP proxy that tunnels HTTPS with CONNECT.
struct Proxy {
    std::string host;
    std::string port;

    bool operator==(const Proxy&) const = default;
};

// "http://host[:port]" or "host:port"; credentials, other schemes and bad ports give nullopt.
std::optional<Proxy> parse_proxy(std::string_view value);

// Whether `host` matches a bypass list: entries separated by commas, semicolons or spaces, each "*", a host, or a
// domain written "example.com", ".example.com" or "*.example.com" that also covers its subdomains. "<local>" covers
// hosts without a dot.
bool bypasses_proxy(std::string_view host, std::string_view bypass);

// The proxy for an HTTPS request to `host`, or nullopt to connect directly. HTTPS_PROXY or ALL_PROXY (either case)
// wins, with NO_PROXY; otherwise the system's HTTPS proxy on macOS and Windows. Loopback hosts are always direct.
// An environment proxy that cannot be used is an error rather than a silent direct connection.
ylt::expected<std::optional<Proxy>, std::string> proxy_for(std::string_view host);

}  // namespace beacon::http
