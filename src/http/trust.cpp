#include <beacon/http/trust.hpp>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <initializer_list>

#if defined(_WIN32)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
    #include <wincrypt.h>
#elif defined(__APPLE__)
    #include "trust_macos.hpp"
#else
    #include <array>
#endif

namespace beacon::http {
namespace {

void add_der(X509_STORE* store, const unsigned char* data, const long size) {
    const unsigned char* cursor = data;
    X509* certificate = d2i_X509(nullptr, &cursor, size);
    if (certificate) {
        (void)X509_STORE_add_cert(store, certificate);
        X509_free(certificate);
    }
}

#if defined(_WIN32)

void add_platform_roots(X509_STORE* store) {
    HCERTSTORE system = CertOpenSystemStoreW(0, L"ROOT");
    if (!system) {
        return;
    }
    for (PCCERT_CONTEXT certificate = CertEnumCertificatesInStore(system, nullptr); certificate;
         certificate = CertEnumCertificatesInStore(system, certificate)) {
        add_der(store, certificate->pbCertEncoded, static_cast<long>(certificate->cbCertEncoded));
    }
    CertCloseStore(system, 0);
}

#elif defined(__APPLE__)

void add_certificate(X509_STORE* store, SecCertificateRef certificate) {
    CFDataRef data = SecCertificateCopyData(certificate);
    if (data) {
        add_der(store, CFDataGetBytePtr(data), static_cast<long>(CFDataGetLength(data)));
        CFRelease(data);
    }
}

// Higher-priority settings override lower domains, including default anchors.
bool trusted_as_root(SecCertificateRef certificate, const bool default_anchor = false) {
    for (const auto domain :
         {kSecTrustSettingsDomainUser, kSecTrustSettingsDomainAdmin, kSecTrustSettingsDomainSystem}) {
        CFArrayRef settings = nullptr;
        const auto status = SecTrustSettingsCopyTrustSettings(certificate, domain, &settings);
        if (status == errSecItemNotFound) {
            continue;
        }
        if (status != errSecSuccess || !settings) {
            return false;
        }
        const auto trusted = detail::unrestricted_root_trust(settings);
        CFRelease(settings);
        // Unspecified preserves only a system default anchor; an existing
        // higher-priority setting never grants trust from a lower domain.
        return trusted.value_or(default_anchor);
    }
    return default_anchor;
}

void add_platform_roots(X509_STORE* store) {
    CFArrayRef anchors = nullptr;
    if (SecTrustCopyAnchorCertificates(&anchors) == errSecSuccess && anchors) {
        for (CFIndex index = 0; index != CFArrayGetCount(anchors); ++index) {
            const auto certificate =
                static_cast<SecCertificateRef>(const_cast<void*>(CFArrayGetValueAtIndex(anchors, index)));
            if (trusted_as_root(certificate, true)) {
                add_certificate(store, certificate);
            }
        }
        CFRelease(anchors);
    }
    // Roots added by an administrator or the user, e.g. by a corporate profile.
    for (const auto domain : {kSecTrustSettingsDomainAdmin, kSecTrustSettingsDomainUser}) {
        CFArrayRef certificates = nullptr;
        if (SecTrustSettingsCopyCertificates(domain, &certificates) != errSecSuccess || !certificates) {
            continue;
        }
        for (CFIndex index = 0; index != CFArrayGetCount(certificates); ++index) {
            auto certificate =
                static_cast<SecCertificateRef>(const_cast<void*>(CFArrayGetValueAtIndex(certificates, index)));
            if (trusted_as_root(certificate)) {
                add_certificate(store, certificate);
            }
        }
        CFRelease(certificates);
    }
}

#else

// Bundle locations of the common distributions; the first readable one wins.
void add_platform_roots(X509_STORE* store) {
    constexpr std::array bundles{
        "/etc/ssl/certs/ca-certificates.crt",                 // Debian, Ubuntu, Arch, Gentoo
        "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",  // Fedora, RHEL
        "/etc/pki/tls/certs/ca-bundle.crt",                   // older Fedora, RHEL
        "/etc/ssl/ca-bundle.pem",                             // openSUSE
        "/etc/ssl/cert.pem",                                  // Alpine
    };
    for (const auto* bundle : bundles) {
        if (X509_STORE_load_file(store, bundle) == 1) {
            return;
        }
    }
}

#endif

}  // namespace

std::size_t add_system_trust(ssl_ctx_st* context) {
    X509_STORE* store = SSL_CTX_get_cert_store(context);
    if (!store) {
        return 0;
    }
    add_platform_roots(store);
    // Failed or duplicate entries must not leak into later OpenSSL error checks.
    ERR_clear_error();
    return static_cast<std::size_t>(sk_X509_OBJECT_num(X509_STORE_get0_objects(store)));
}

}  // namespace beacon::http
