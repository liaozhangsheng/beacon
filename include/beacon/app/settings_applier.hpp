#pragma once

#include <beacon/app/persistence.hpp>
#include <beacon/app/runtime.hpp>
#include <beacon/ui/display.hpp>

#include <filesystem>

namespace beacon {

// Applies edited settings as one transaction: a changed template, game root or language is loaded and validated,
// the settings are saved, and only then does the runtime switch over, so a failure leaves the live run untouched.
class SettingsApplier {
public:
    SettingsApplier(Runtime& runtime, Persistence& persistence, std::filesystem::path asset_root,
                    TemplateResourceLoader loader);

    ylt::expected<void, Error> apply(const Settings& applied, const Settings& requested);

private:
    Runtime& runtime_;
    Persistence& persistence_;
    std::filesystem::path asset_root_;
    TemplateResourceLoader loader_;
};

}  // namespace beacon
