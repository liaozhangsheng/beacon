#pragma once

#include <beacon/core/model.hpp>
#include <beacon/io/file.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace beacon {

struct Localization {
    std::unordered_map<std::string, std::string> selected;
    std::unordered_map<std::string, std::string> defaults;

    [[nodiscard]] std::string_view text(std::string_view key) const;
};

enum class LayoutSource : std::uint8_t {
    Nodes,
    Prefix,
    Children,
};

struct LayoutGroup {
    std::string id;
    LayoutSource source = LayoutSource::Nodes;
    std::vector<std::string> nodes;
    std::vector<std::string> exclude;
    float x = 0.0F;
    float y = 0.0F;
    float width = 1.0F;
    float height = 1.0F;
    float margin = 0.0F;
    float padding = 0.0F;

    bool operator==(const LayoutGroup&) const = default;
};

struct Layout {
    std::vector<Error> warnings;
    std::vector<LayoutGroup> main;
    std::vector<LayoutGroup> overlay;

    bool operator==(const Layout&) const = default;
};

struct TemplateResources {
    std::shared_ptr<const CompiledTemplate> compiled;
    std::shared_ptr<const Localization> localization;
    std::shared_ptr<const Layout> layout;
};

using IconFiles = std::vector<std::pair<std::filesystem::path, FileStamp>>;
ylt::expected<IconFiles, Error> icon_file_stamps(const CompiledTemplate& compiled);

class TemplateResourceLoader {
public:
    TemplateResourceLoader() = default;
    TemplateResourceLoader(std::filesystem::path template_path, std::filesystem::path asset_root,
                           TemplateResources resources);
    // A language-only change reuses validated rules, layout and images.
    ylt::expected<TemplateResources, Error> load(const std::filesystem::path& template_path,
                                                 const std::filesystem::path& asset_root, std::string_view language,
                                                 bool language_only = false);

private:
    std::filesystem::path template_path_;
    std::filesystem::path asset_root_;
    TemplateResources resources_;
    IconFiles icons_;
};

ylt::expected<Localization, Error> compile_localization_json(std::string_view default_json,
                                                             std::string_view selected_json = {});
ylt::expected<Layout, Error> compile_layout_json(std::string_view json, const CompiledTemplate& compiled);
std::vector<std::string> available_template_languages(const std::filesystem::path& template_path);
std::vector<std::uint32_t> resolve_layout_nodes(const LayoutGroup& group, const CompiledTemplate& compiled,
                                                std::size_t result_count);
std::string display_label(std::uint32_t node, const CompiledTemplate& compiled, const Localization& localization);
ylt::expected<void, Error> resolve_icon_paths(CompiledTemplate& compiled, const std::filesystem::path& template_root,
                                              const std::filesystem::path& asset_root);
ylt::expected<TemplateResources, Error> load_template_resources(const std::filesystem::path& template_path,
                                                                const std::filesystem::path& asset_root,
                                                                std::string_view language = {});

}  // namespace beacon
