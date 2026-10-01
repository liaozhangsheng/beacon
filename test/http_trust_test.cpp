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
