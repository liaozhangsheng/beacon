#include <beacon/update/download.hpp>
#include <beacon/io/file.hpp>

#include "common.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <memory>
#include <utility>

namespace beacon::update {
namespace {

using Digest = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;

Digest new_digest() {
    Digest digest(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (digest && EVP_DigestInit_ex(digest.get(), EVP_sha256(), nullptr) != 1)
        digest.reset();
    return digest;
}

ylt::expected<std::string, Error> finish(EVP_MD_CTX* digest) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> bytes{};
    unsigned int size = 0;
    if (EVP_DigestFinal_ex(digest, bytes.data(), &size) != 1)
        return failure(ErrorCode::Internal, "cannot finalize SHA-256");
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(size * 2);
    for (unsigned int index = 0; index < size; ++index) {
        result.push_back(digits[bytes[index] >> 4]);
        result.push_back(digits[bytes[index] & 0xfU]);
    }
    return result;
}

}  // namespace

ylt::expected<std::string, Error> sha256_file(const std::filesystem::path& path, const std::uint64_t max_bytes) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || !std::filesystem::is_regular_file(path, ec))
        return failure(ErrorCode::Io, "cannot read artifact", path_to_utf8(path));
    if (size > max_bytes)
        return failure(ErrorCode::SecurityLimit, "artifact exceeds size limit", path_to_utf8(path));
    std::ifstream input(path, std::ios::binary);
    auto digest = new_digest();
    if (!input || !digest)
        return failure(ErrorCode::Io, "cannot hash artifact", path_to_utf8(path));
    std::array<char, 64U * 1024U> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0 && EVP_DigestUpdate(digest.get(), buffer.data(), static_cast<std::size_t>(count)) != 1)
            return failure(ErrorCode::Internal, "cannot hash artifact", path_to_utf8(path));
    }
    if (!input.eof())
        return failure(ErrorCode::Io, "cannot read artifact", path_to_utf8(path));
    return finish(digest.get());
}

ylt::expected<std::filesystem::path, Error> download_file(const std::string_view url,
                                                          const std::filesystem::path& destination,
                                                          const std::uint64_t expected_size,
                                                          const std::string_view expected_sha256, http::Limits limits) {
    if (!valid_sha256(expected_sha256))
        return failure(ErrorCode::Validation, "manifest contains an invalid SHA-256", "sha256");
    if (expected_size > limits.max_bytes)
        return failure(ErrorCode::SecurityLimit, "artifact exceeds size limit");
    const auto expected_hash = lower(std::string(expected_sha256));
    std::error_code ec;
    if (std::filesystem::file_size(destination, ec) == expected_size && !ec) {
        if (auto hash = sha256_file(destination, expected_size); hash && *hash == expected_hash)
            return destination;
    }
    std::filesystem::remove(destination, ec);
    std::filesystem::create_directories(destination.parent_path(), ec);
    if (ec)
        return failure(ErrorCode::Io, "cannot create artifact cache", path_to_utf8(destination.parent_path()));

    auto partial = destination;
    partial += ".part";
    const auto discard = [&](Error error) -> ylt::expected<std::filesystem::path, Error> {
        std::error_code ignored;
        std::filesystem::remove(partial, ignored);
        return ylt::unexpected<Error>{std::move(error)};
    };
    auto digest = new_digest();
    std::ofstream output(partial, std::ios::binary | std::ios::trunc);
    if (!output || !digest)
        return discard(make_error(ErrorCode::Io, "cannot create partial artifact", path_to_utf8(partial)));
    // The manifest size bounds the body, so a larger response fails early.
    limits.max_bytes = expected_size;
    auto received = http::get(url, limits, [&](const std::string_view bytes) {
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        return output.good() && EVP_DigestUpdate(digest.get(), bytes.data(), bytes.size()) == 1;
    });
    output.close();
    if (!received)
        return discard(std::move(received.error()));
    if (!output)
        return discard(make_error(ErrorCode::Io, "cannot write partial artifact", path_to_utf8(partial)));
    if (*received != expected_size)
        return discard(make_error(ErrorCode::Validation, "download size does not match manifest", std::string(url)));
    auto hash = finish(digest.get());
    if (!hash || *hash != expected_hash)
        return discard(make_error(ErrorCode::Validation, "download SHA-256 does not match manifest", std::string(url)));
    std::filesystem::rename(partial, destination, ec);
    if (ec)
        return discard(make_error(ErrorCode::Io, "cannot commit cached artifact", path_to_utf8(destination)));
    return destination;
}

}  // namespace beacon::update
