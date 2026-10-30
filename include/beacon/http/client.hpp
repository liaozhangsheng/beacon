#pragma once

#include <beacon/core/model.hpp>

#include <cstdint>
#include <functional>
#include <stop_token>
#include <string>
#include <string_view>

namespace beacon::http {

struct Limits {
    std::uint64_t max_bytes = 512U * 1024U * 1024U;
    long timeout_seconds = 120;
    long connect_timeout_seconds = 20;
    long max_redirects = 5;
};

// Receives the response body in order; returning false aborts the transfer.
using BodySink = std::function<bool(std::string_view)>;

// Accepts absolute HTTPS URLs whose host is unambiguous for both URI parsing
// and TLS verification: no user info, fragment, percent-encoded host or
// control characters.
bool is_https_url(std::string_view url);

// Performs an HTTPS GET verified against the operating system's trusted roots.
// Redirects are followed only to HTTPS URLs, and only a 2xx body reaches sink.
// Fails when the body exceeds max_bytes, the time limit passes or stop is
// requested. Returns the number of body bytes.
ylt::expected<std::uint64_t, Error> get(std::string_view url, const Limits& limits, const BodySink& sink,
                                        std::stop_token stop = {});

// Returns the exact response body, e.g. for verifying a detached signature.
ylt::expected<std::string, Error> get_text(std::string_view url, const Limits& limits, std::stop_token stop = {});

}  // namespace beacon::http
