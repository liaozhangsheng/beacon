#include <beacon/http/proxy.hpp>
#include <beacon/http/trust.hpp>

#include <catch2/catch_test_macros.hpp>

#include <openssl/ssl.h>

#include <memory>

#if defined(__APPLE__)
    #include "../src/http/trust_macos.hpp"
#endif

// Packaged builds cannot rely on OpenSSL's compiled-in certificate paths.
TEST_CASE("HTTPS clients load the operating system's trusted roots", "[http]") {
    const std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> context(SSL_CTX_new(TLS_client_method()), SSL_CTX_free);
    REQUIRE(context);
    CHECK(beacon::http::add_system_trust(context.get()) > 0);
}

#if defined(__APPLE__)
TEST_CASE("macOS constrained roots are not imported as global HTTPS trust", "[http]") {
    const auto settings = CFArrayCreateMutable(nullptr, 0, &kCFTypeArrayCallBacks);
    REQUIRE(settings);
    CHECK(beacon::http::detail::unrestricted_root_trust(settings) == true);
    const auto entry =
        CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    REQUIRE(entry);
    CFArrayAppendValue(settings, entry);
    CHECK(beacon::http::detail::unrestricted_root_trust(settings) == true);
    for (const auto key : {kSecTrustSettingsPolicy, kSecTrustSettingsPolicyString, kSecTrustSettingsApplication,
                           kSecTrustSettingsKeyUsage, kSecTrustSettingsAllowedError}) {
        CFDictionarySetValue(entry, key, CFSTR("restriction"));
        CHECK(beacon::http::detail::unrestricted_root_trust(settings) == false);
        CFDictionaryRemoveValue(entry, key);
    }
    for (const SInt32 result : {kSecTrustSettingsResultTrustRoot, kSecTrustSettingsResultTrustAsRoot,
                                kSecTrustSettingsResultDeny, kSecTrustSettingsResultUnspecified}) {
        const auto value = CFNumberCreate(nullptr, kCFNumberSInt32Type, &result);
        REQUIRE(value);
        CFDictionarySetValue(entry, kSecTrustSettingsResult, value);
        if (result == kSecTrustSettingsResultUnspecified) {
            CHECK_FALSE(beacon::http::detail::unrestricted_root_trust(settings).has_value());
            CFDictionarySetValue(entry, kSecTrustSettingsPolicyString, CFSTR("example.invalid"));
            CHECK_FALSE(beacon::http::detail::unrestricted_root_trust(settings).has_value());
        } else {
            CHECK(beacon::http::detail::unrestricted_root_trust(settings) ==
                  (result == kSecTrustSettingsResultTrustRoot || result == kSecTrustSettingsResultTrustAsRoot));
        }
        CFRelease(value);
    }
    // Unspecified entries do not cancel a separate, unconditional grant.
    const auto unrestricted =
        CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    REQUIRE(unrestricted);
    CFArrayAppendValue(settings, unrestricted);
    CHECK(beacon::http::detail::unrestricted_root_trust(settings) == true);
    CFRelease(unrestricted);
    CFRelease(entry);
    CFRelease(settings);
}
#endif

TEST_CASE("proxy settings parse to a CONNECT proxy", "[http]") {
    using beacon::http::parse_proxy;
    using beacon::http::Proxy;
    CHECK(parse_proxy("http://127.0.0.1:7890") == Proxy{.host = "127.0.0.1", .port = "7890"});
    CHECK(parse_proxy(" proxy.example:8080/ ") == Proxy{.host = "proxy.example", .port = "8080"});
    CHECK(parse_proxy("HTTP://[::1]:3128") == Proxy{.host = "::1", .port = "3128"});
    CHECK(parse_proxy("http://proxy.example") == Proxy{.host = "proxy.example", .port = "80"});
    CHECK(parse_proxy("http://[::1]/") == Proxy{.host = "::1", .port = "80"});
    CHECK_FALSE(parse_proxy("[::1]"));
    CHECK_FALSE(parse_proxy("socks5://127.0.0.1:7890"));
    CHECK_FALSE(parse_proxy("http://user:secret@proxy:8080"));
    CHECK_FALSE(parse_proxy("proxy"));
    CHECK_FALSE(parse_proxy("proxy:0"));
    CHECK_FALSE(parse_proxy("proxy:65536"));
    CHECK_FALSE(parse_proxy("proxy:80x"));
    CHECK_FALSE(parse_proxy(":8080"));
}

TEST_CASE("proxy bypass lists match hosts and their subdomains", "[http]") {
    using beacon::http::bypasses_proxy;
    CHECK(bypasses_proxy("docs.google.com", "localhost, google.com"));
    CHECK(bypasses_proxy("google.com", ".google.com"));
    CHECK(bypasses_proxy("docs.google.com", "*.google.com;example.org"));
    CHECK(bypasses_proxy("DOCS.Google.com", "google.com"));
    CHECK(bypasses_proxy("anything", "*"));
    CHECK(bypasses_proxy("intranet", "<local>"));
    CHECK_FALSE(bypasses_proxy("docs.google.com", "<local>"));
    CHECK_FALSE(bypasses_proxy("notgoogle.com", "google.com"));
    CHECK_FALSE(bypasses_proxy("api.mojang.com", ""));
}
