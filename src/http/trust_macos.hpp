#pragma once

#include <Security/Security.h>

#include <optional>

namespace beacon::http::detail {

inline std::optional<bool> unrestricted_root_trust(CFArrayRef settings) {
    const auto count = CFArrayGetCount(settings);
    std::optional<bool> trusted;
    if (count == 0) {
        return true;
    }
    for (CFIndex index = 0; index != count; ++index) {
        const auto entry = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(settings, index));
        const auto value = static_cast<CFNumberRef>(CFDictionaryGetValue(entry, kSecTrustSettingsResult));
        SInt32 result = kSecTrustSettingsResultTrustRoot;
        if (value && !CFNumberGetValue(value, kCFNumberSInt32Type, &result)) {
            return false;
        }
        // Unspecified grants no trust; the caller may keep a default anchor.
        // Its allowed-error fields are not applied to OpenSSL.
        if (result == kSecTrustSettingsResultUnspecified) {
            continue;
        }
        // OpenSSL's store cannot express Keychain usage constraints. Only
        // import settings that authorize trust without any other fields.
        if (CFDictionaryGetCount(entry) != (value ? 1 : 0)) {
            return false;
        }
        if (result != kSecTrustSettingsResultTrustRoot && result != kSecTrustSettingsResultTrustAsRoot) {
            return false;
        }
        trusted = true;
    }
    return trusted;
}

}  // namespace beacon::http::detail
