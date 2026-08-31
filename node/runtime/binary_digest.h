#pragma once

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>

#include <openssl/evp.h>

namespace flexedge::node {

inline std::string hexEncode(const unsigned char* input, std::size_t size) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string result;
    result.reserve(size * 2);
    for (std::size_t index = 0; index < size; ++index) {
        result.push_back(kHex[(input[index] >> 4) & 0x0f]);
        result.push_back(kHex[input[index] & 0x0f]);
    }
    return result;
}

inline std::string binarySha256(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("node binary is unavailable");
    }

    using DigestContext = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
    DigestContext context(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
    if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) {
        throw std::runtime_error("failed to initialize node binary digest");
    }

    std::array<char, 64 * 1024> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0 &&
            EVP_DigestUpdate(context.get(), buffer.data(), static_cast<std::size_t>(count)) != 1) {
            throw std::runtime_error("failed to hash node binary");
        }
    }
    if (input.bad()) {
        throw std::runtime_error("failed to read node binary");
    }

    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digestSize = 0;
    if (EVP_DigestFinal_ex(context.get(), digest.data(), &digestSize) != 1) {
        throw std::runtime_error("failed to finalize node binary digest");
    }
    return hexEncode(digest.data(), digestSize);
}

} // namespace flexedge::node
