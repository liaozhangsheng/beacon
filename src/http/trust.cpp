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

    #include <string_view>
    #include <vector>
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

// Roots Windows limits to other purposes, such as code signing or e-mail, are not HTTPS anchors.
bool trusted_for_server_auth(PCCERT_CONTEXT certificate) {
    DWORD size = 0;
    if (!CertGetEnhancedKeyUsage(certificate, 0, nullptr, &size)) {
        return GetLastError() == static_cast<DWORD>(CRYPT_E_NOT_FOUND);
    }
    std::vector<unsigned char> buffer(size);
    auto* usage = reinterpret_cast<CERT_ENHKEY_USAGE*>(buffer.data());
    if (!CertGetEnhancedKeyUsage(certificate, 0, usage, &size)) {
        return false;
    }
    // An empty list means every purpose unless Windows reports that some were disabled.
    if (usage->cUsageIdentifier == 0) {
        return GetLastError() == static_cast<DWORD>(CRYPT_E_NOT_FOUND);
    }
    for (DWORD index = 0; index != usage->cUsageIdentifier; ++index) {
        if (std::string_view(usage->rgpszUsageIdentifier[index]) == szOID_PKIX_KP_SERVER_AUTH) {
            return true;
        }
    }
    return false;
}

void add_store_roots(X509_STORE* store, HCERTSTORE roots, HCERTSTORE disallowed) {
    if (!roots) {
        return;
    }
    for (PCCERT_CONTEXT certificate = CertEnumCertificatesInStore(roots, nullptr); certificate;
         certificate = CertEnumCertificatesInStore(roots, certificate)) {
        if (disallowed) {
            if (PCCERT_CONTEXT found = CertFindCertificateInStore(disallowed, X509_ASN_ENCODING, 0, CERT_FIND_EXISTING,
                                                                  certificate, nullptr)) {
                CertFreeCertificateContext(found);
                continue;
            }
        }
        if (trusted_for_server_auth(certificate)) {
            add_der(store, certificate->pbCertEncoded, static_cast<long>(certificate->cbCertEncoded));
        }
    }
    CertCloseStore(roots, 0);
}

// ROOT holds the user's, the machine's and policy-deployed roots. Third-party roots from Microsoft's root program
// live in the machine's AuthRoot store, which a fresh system fills on demand; browsers see them through CryptoAPI,
// so Beacon reads them too.
void add_platform_roots(X509_STORE* store) {
    constexpr DWORD open_flags = CERT_STORE_READONLY_FLAG | CERT_STORE_OPEN_EXISTING_FLAG;
    HCERTSTORE disallowed = CertOpenSystemStoreW(0, L"Disallowed");
    add_store_roots(store, CertOpenSystemStoreW(0, L"ROOT"), disallowed);
    add_store_roots(
        store, CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_LOCAL_MACHINE | open_flags, L"AuthRoot"),
        disallowed);
    if (disallowed) {
        CertCloseStore(disallowed, 0);
    }
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
