#include <beacon/http/client.hpp>
#include <beacon/http/proxy.hpp>
#include <beacon/http/trust.hpp>

#include <ylt/coro_http/coro_http_client.hpp>

#include <algorithm>
#include <array>
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

// Relays one loopback connection through an HTTP proxy's CONNECT tunnel. The HTTP client connects to endpoint()
// and runs TLS with the site through it, so the certificate is still checked against the site, not the proxy.
class ProxyTunnel {
public:
    ProxyTunnel(asio::io_context& context, Proxy proxy, std::string target)
        : proxy_(std::move(proxy)), target_(std::move(target)), acceptor_(context), resolver_(context), local_(context),
          upstream_(context) {
        std::error_code ec;
        acceptor_.open(asio::ip::tcp::v4(), ec);
        if (!ec)
            acceptor_.bind({asio::ip::address_v4::loopback(), 0}, ec);
        if (!ec)
            acceptor_.listen(1, ec);
        if (ec)
            error_ = "cannot open the proxy relay: " + ec.message();
    }

    [[nodiscard]] bool ready() const {
        return error_.empty();
    }
    [[nodiscard]] asio::ip::tcp::endpoint endpoint() const {
        std::error_code ec;
        return acceptor_.local_endpoint(ec);
    }
    // Why the tunnel failed, once it has.
    [[nodiscard]] const std::string& error() const {
        return error_;
    }

    void start() {
        run().start([](auto&&) {});
    }
    void close() {
        std::error_code ec;
        acceptor_.close(ec);
        resolver_.cancel();
        local_.close(ec);
        upstream_.close(ec);
    }

private:
    async_simple::coro::Lazy<void> run() {
        // Only the HTTP client connects here, and only once.
        if (co_await coro_io::async_io<std::error_code>(
                [&](auto&& callback) {
                    acceptor_.async_accept(local_, std::move(callback));
                },
                acceptor_))
            co_return close();
        std::error_code ignored;
        acceptor_.close(ignored);
        auto [resolve_error, resolved] =
            co_await coro_io::async_io<std::pair<std::error_code, asio::ip::tcp::resolver::results_type>>(
                [&](auto&& callback) {
                    resolver_.async_resolve(proxy_.host, proxy_.port, std::move(callback));
                },
                resolver_);
        if (resolve_error) {
            fail("proxy lookup failed: " + resolve_error.message());
            co_return;
        }
        auto [connect_error, connected] =
            co_await coro_io::async_io<std::pair<std::error_code, asio::ip::tcp::endpoint>>(
                [&](auto&& callback) {
                    asio::async_connect(upstream_, resolved, std::move(callback));
                },
                upstream_);
        if (connect_error) {
            fail("cannot connect to the proxy " + proxy_.host + ":" + proxy_.port + ": " + connect_error.message());
            co_return;
        }
        const std::string request = "CONNECT " + target_ + " HTTP/1.1\r\nHost: " + target_ + "\r\n\r\n";
        if (auto [error, size] = co_await coro_io::async_write(upstream_, asio::buffer(request)); error) {
            fail("proxy connection failed: " + error.message());
            co_return;
        }
        asio::streambuf reply(16384);
        const auto [read_error, header_size] = co_await coro_io::async_read_until(upstream_, reply, "\r\n\r\n");
        if (read_error) {
            fail("proxy connection failed: " + read_error.message());
            co_return;
        }
        const std::string_view header(static_cast<const char*>(reply.data().data()), header_size);
        const auto status = header.substr(0, header.find("\r\n"));
        // "HTTP/1.1 200 Connection established"
        const bool established = status.starts_with("HTTP/1.") && status.size() >= 12 &&
                                 status.substr(8, 4) == " 200" && (status.size() == 12 || status[12] == ' ');
        if (!established) {
            fail("proxy refused the tunnel: " + std::string(status));
            co_return;
        }
        reply.consume(header_size);
        // Bytes the proxy sent past its reply already belong to the site.
        if (reply.size() > 0) {
            if (auto [error, size] = co_await coro_io::async_write(local_, reply.data()); error)
                co_return close();
        }
        relay(upstream_, local_).start([](auto&&) {});
        co_await relay(local_, upstream_);
    }

    async_simple::coro::Lazy<void> relay(asio::ip::tcp::socket& from, asio::ip::tcp::socket& to) {
        std::array<char, 16384> buffer{};
        while (true) {
            auto [read_error, size] = co_await coro_io::async_read_some(from, asio::buffer(buffer));
            if (read_error)
                break;
            if (auto [write_error, written] = co_await coro_io::async_write(to, asio::buffer(buffer.data(), size));
                write_error)
                break;
        }
        close();
    }

    void fail(std::string message) {
        error_ = std::move(message);
        close();
    }

    Proxy proxy_;
    // "host:port" of the site.
    std::string target_;
    asio::ip::tcp::acceptor acceptor_;
    asio::ip::tcp::resolver resolver_;
    asio::ip::tcp::socket local_;
    asio::ip::tcp::socket upstream_;
    std::string error_;
};

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
    // One per proxied hop; each lives until the context stops, since its relay may still be winding down.
    std::vector<std::unique_ptr<ProxyTunnel>> tunnels;
    bool timed_out = false;
    bool cancelled = false;
    bool exceeded = false;
    bool sink_failed = false;
    std::uint64_t total = 0;
    const auto close_tunnels = [&] {
        for (const auto& tunnel : tunnels)
            tunnel->close();
    };
    const auto abort = [&] {
        resolver.cancel();
        if (client)
            client->close();
        close_tunnels();
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
            std::vector<asio::ip::tcp::endpoint> endpoints;
            ProxyTunnel* tunnel = nullptr;
            auto proxy = proxy_for(uri.get_host());
            if (!proxy) {
                result = ylt::unexpected<Error>{failure(ErrorCode::Validation, std::move(proxy.error()))};
                co_return;
            }
            if (*proxy) {
                tunnels.push_back(std::make_unique<ProxyTunnel>(
                    context, std::move(**proxy), std::string(uri.get_host()) + ":" + std::string(uri.get_port())));
                tunnel = tunnels.back().get();
                if (!tunnel->ready()) {
                    result = ylt::unexpected<Error>{failure(ErrorCode::Io, tunnel->error())};
                    co_return;
                }
                tunnel->start();
                endpoints.push_back(tunnel->endpoint());
            } else {
                // Own the resolver so cancellation also covers DNS, before a socket exists.
                auto [error, resolved] =
                    co_await coro_io::async_io<std::pair<std::error_code, asio::ip::tcp::resolver::results_type>>(
                        [&](auto&& callback) {
                            resolver.async_resolve(uri.get_host(), uri.get_port(), std::move(callback));
                        },
                        resolver);
                if (error) {
                    result =
                        ylt::unexpected<Error>{failure(ErrorCode::Io, "HTTPS DNS lookup failed: " + error.message())};
                    co_return;
                }
                for (const auto& entry : resolved)
                    endpoints.push_back(entry.endpoint());
            }
            if (timed_out || cancelled)
                co_return;
            const auto connected = co_await client->connect(current, &endpoints);
            if (connected.net_err) {
                result = ylt::unexpected<Error>{failure(
                    ErrorCode::Io, tunnel != nullptr && !tunnel->error().empty()
                                       ? "HTTPS " + tunnel->error()
                                       : "HTTPS connection or TLS handshake failed: " + connected.net_err.message())};
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
            close_tunnels();
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
