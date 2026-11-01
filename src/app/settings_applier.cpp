#include <beacon/app/settings_applier.hpp>

#include <optional>
#include <utility>

namespace beacon {

SettingsApplier::SettingsApplier(Runtime& runtime, Persistence& persistence, std::filesystem::path asset_root,
                                 TemplateResourceLoader loader)
    : runtime_(runtime), persistence_(persistence), asset_root_(std::move(asset_root)), loader_(std::move(loader)) {}

ylt::expected<void, Error> SettingsApplier::apply(const Settings& applied, const Settings& requested) {
    auto loader = loader_;
    std::optional<RuntimeConfiguration> prepared;
    if (requested.game_root != applied.game_root || requested.template_path != applied.template_path ||
        requested.language != applied.language) {
        const bool language_only =
            requested.template_path == applied.template_path && requested.language != applied.language;
        auto resources = loader.load(requested.template_path, asset_root_, requested.language, language_only);
        if (!resources)
            return ylt::unexpected<Error>{std::move(resources.error())};
        auto candidate = Runtime::prepare_configuration(requested.game_root, resources->compiled,
                                                        resources->localization, resources->layout);
        if (!candidate)
            return ylt::unexpected<Error>{std::move(candidate.error())};
        prepared = std::move(*candidate);
    }
    if (auto saved = persistence_.save_settings(requested); !saved)
        return saved;
    if (prepared)
        runtime_.commit_configuration(std::move(*prepared));
    loader_ = std::move(loader);
    return {};
}

}  // namespace beacon
