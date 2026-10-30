#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>

namespace beacon {

// Arguments are already split by the operating system; never log the command line (it may contain tokens).
std::optional<std::filesystem::path> minecraft_game_directory(std::span<const std::string> arguments);
std::optional<std::filesystem::path> foreground_minecraft_directory();

}  // namespace beacon
