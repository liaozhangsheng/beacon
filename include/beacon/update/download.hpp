#pragma once

#include <beacon/core/model.hpp>
#include <beacon/http/client.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace beacon::update {

// Downloads an artifact into destination, reusing an existing file only when
// its size and SHA-256 match. destination must be a cache path controlled by
// the caller; a partial download is written next to it.
ylt::expected<std::filesystem::path, Error> download_file(std::string_view url,
                                                          const std::filesystem::path& destination,
                                                          std::uint64_t expected_size, std::string_view expected_sha256,
                                                          http::Limits limits = {});

ylt::expected<std::string, Error> sha256_file(const std::filesystem::path& path,
                                              std::uint64_t max_bytes = 512U * 1024U * 1024U);

}  // namespace beacon::update
