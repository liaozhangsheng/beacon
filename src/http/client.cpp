#include <beacon/http/client.hpp>
#include <beacon/http/trust.hpp>

#include <ylt/coro_http/coro_http_client.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace beacon::http {
namespace {

Error failure(const ErrorCode code, std::string message) {
    return {.code = code, .message = std::move(message), .context = "https"};
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

bool redirect_status(const int status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

// Resolve the redirect before creating a fresh client, so SNI and hostname
// verification always refer to this hop's host.
std::string redirect_url(const std::string& current, const std::string_view location) {
    if (location.empty())
        return {};
    if (location.starts_with("//"))
        return "https:" + std::string(location);
    if (location.find("://") != std::string_view::npos)
        return std::string(location);
    const auto authority_end = current.find_first_of("/?#", 8);
    const auto origin = current.substr(0, authority_end);
    if (location.front() == '/')
        return origin + std::string(location);
    auto base = current.substr(0, current.find_first_of("?#"));
    if (location.front() == '?')
        return base + (base == origin ? "/" : "") + std::string(location);
    // A colon in the first path segment denotes a scheme, not a relative path.
    if (location.substr(0, location.find('/')).find(':') != std::string_view::npos)
        return {};
    if (base == origin)
        base += '/';
    else
        base.resize(base.rfind('/') + 1);
    return base + std::string(location);
}

}  // namespace

bool is_https_url(const std::string_view url) {
    if (url.size() > 16384 || !url.starts_with("https://") || !std::all_of(url.begin(), url.end(), [](unsigned char c) {
            return c > 32 && c < 127 && c != '\\';
        }))
        return false;
    const std::string storage(url);
    coro_http::uri_t uri;
    if (!uri.parse_from(storage.c_str()) || uri.schema != "https" || uri.host.empty() || !uri.uinfo.empty() ||
        !uri.fragment.empty())
        return false;
    return uri.host.find('%') == std::string_view::npos;
}

ylt::expected<std::uint64_t, Error> get(const std::string_view url, const Limits& limits, const BodySink& sink,
                                        const std::stop_token stop) {
    if (!is_https_url(url))
        return ylt::unexpected<Error>{failure(ErrorCode::Validation, "URL must use HTTPS")};
    if (limits.max_bytes == 0 || limits.max_bytes > static_cast<std::uint64_t>(INT64_MAX) ||
        limits.timeout_seconds <= 0 || limits.connect_timeout_seconds <= 0 || limits.max_redirects < 0)
        return ylt::unexpected<Error>{failure(ErrorCode::Validation, "invalid HTTPS limits")};

    asio::io_context context;
    asio::ip::tcp::resolver resolver(context);
    std::unique_ptr<coro_http::coro_http_client> client;
    bool timed_out = false;
    bool cancelled = false;
    bool exceeded = false;
    bool sink_failed = false;
    std::uint64_t total = 0;
    const auto abort = [&] {
        resolver.cancel();
        if (client)
            client->close();
    };
    asio::steady_timer deadline(context, std::chrono::seconds(limits.timeout_seconds));
    deadline.async_wait([&](const std::error_code& error) {
        if (!error) {
            timed_out = true;
            abort();
        }
    });
    ylt::expected<std::uint64_t, Error> result = ylt::unexpected<Error>{failure(ErrorCode::Io, "HTTPS request failed")};
    const auto request = [&]() -> async_simple::coro::Lazy<void> {
        std::string current(url);
        for (long hop = 0; hop <= limits.max_redirects; ++hop) {
            if (timed_out || cancelled)
                co_return;
            coro_http::uri_t uri;
            uri.parse_from(current.c_str());
            client = std::make_unique<coro_http::coro_http_client>(context.get_executor());
            client->ssl_context_setup = [](asio::ssl::context& ssl) {
                (void)add_system_trust(ssl.native_handle());
            };
            if (!client->init_ssl(asio::ssl::verify_peer, "", std::string(uri.host))) {
                result = ylt::unexpected<Error>{failure(ErrorCode::Io, "HTTPS TLS initialization failed")};
                co_return;
            }
            client->enable_auto_redirect(false);
            client->set_max_http_body_size(static_cast<std::int64_t>(limits.max_bytes));
            client->set_conn_timeout(std::chrono::seconds(limits.connect_timeout_seconds));
            client->set_req_timeout(std::chrono::seconds(limits.timeout_seconds));
            client->add_header("User-Agent", "Beacon");
            client->add_header("Accept-Encoding", "identity");
            // Own the resolver so cancellation also covers DNS, before a socket exists.
            auto [error, resolved] =
                co_await coro_io::async_io<std::pair<std::error_code, asio::ip::tcp::resolver::results_type>>(
                    [&](auto&& callback) {
                        resolver.async_resolve(uri.get_host(), uri.get_port(), std::move(callback));
                    },
                    resolver);
            if (error) {
                result = ylt::unexpected<Error>{failure(ErrorCode::Io, "HTTPS DNS lookup failed: " + error.message())};
                co_return;
            }
            if (timed_out || cancelled)
                co_return;
            std::vector<asio::ip::tcp::endpoint> endpoints;
            for (const auto& entry : resolved)
                endpoints.push_back(entry.endpoint());
            const auto connected = co_await client->connect(current, &endpoints);
            if (connected.net_err) {
                result = ylt::unexpected<Error>{
                    failure(ErrorCode::Io, "HTTPS connection or TLS handshake failed: " + connected.net_err.message())};
                co_return;
            }
            if (timed_out || cancelled)
                co_return;
            coro_http::req_context<> body;
            body.resp_body_sink = [&](const std::string_view bytes) {
                if (bytes.size() > limits.max_bytes - total) {
                    exceeded = true;
                    return false;
                }
                if (!sink(bytes)) {
                    sink_failed = true;
                    return false;
                }
                total += bytes.size();
                return true;
            };
            const auto response = co_await client->async_request(current, coro_http::http_method::GET, std::move(body));
            if (response.net_err) {
                result = ylt::unexpected<Error>{
                    failure(ErrorCode::Io, "HTTPS response failed: " + response.net_err.message())};
                co_return;
            }
            if (timed_out || cancelled)
                co_return;
            if (response.status >= 200 && response.status < 300) {
                result = total;
                co_return;
            }
            if (!redirect_status(response.status)) {
                result = ylt::unexpected<Error>{failure(ErrorCode::Io, "server returned a non-success HTTP status")};
                co_return;
            }
            std::string location;
            for (const auto& header : response.resp_headers) {
                if (lower(std::string(header.name)) == "location") {
                    if (!location.empty())
                        co_return;
                    location = header.value;
                }
            }
            current = redirect_url(current, location);
            if (!is_https_url(current)) {
                result = ylt::unexpected<Error>{failure(ErrorCode::Validation, "redirect must use a valid HTTPS URL")};
                co_return;
            }
        }
        result = ylt::unexpected<Error>{failure(ErrorCode::SecurityLimit, "too many HTTPS redirects")};
    };
    {
        std::stop_callback on_stop(stop, [&] {
            asio::post(context, [&] {
                cancelled = true;
                abort();
            });
        });
        request().start([&](auto&& completed) {
            if (completed.hasError())
                result = ylt::unexpected<Error>{failure(ErrorCode::Io, "HTTPS request failed")};
            deadline.cancel();
        });
        context.run();
    }
    // Drain a cancellation posted just as the request completed, before the client is destroyed.
    context.restart();
    context.poll();
    if (cancelled || stop.stop_requested())
        return ylt::unexpected<Error>{failure(ErrorCode::Io, "HTTPS request was cancelled")};
    if (timed_out)
        return ylt::unexpected<Error>{failure(ErrorCode::Io, "HTTPS request timed out")};
    if (exceeded)
        return ylt::unexpected<Error>{failure(ErrorCode::SecurityLimit, "response exceeds size limit")};
    if (sink_failed)
        return ylt::unexpected<Error>{failure(ErrorCode::Io, "cannot store response data")};
    return result;
}

ylt::expected<std::string, Error> get_text(const std::string_view url, const Limits& limits,
                                           const std::stop_token stop) {
    std::string body;
    auto result = get(
        url, limits,
        [&](const std::string_view bytes) {
            body.append(bytes);
            return true;
        },
        stop);
    if (!result)
        return ylt::unexpected<Error>{std::move(result.error())};
    return body;
}

}  // namespace beacon::http
