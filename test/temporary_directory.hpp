#pragma once

#include <filesystem>
#include <random>
#include <string>

namespace beacon::test {

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        std::random_device random;
        const auto root = std::filesystem::temp_directory_path();
        do {
            path = root / ("beacon-test-" + std::to_string(random()));
        } while (!std::filesystem::create_directory(path));
    }
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    std::filesystem::path path;
};

}  // namespace beacon::test
