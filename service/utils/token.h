#pragma once

#include <array>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include "service/utils/sensitive_string.h"

namespace service::utils {

inline std::string hexEncode(const unsigned char* input, std::size_t size) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result(size * 2, '\0');
    for (std::size_t index = 0; index < size; ++index) {
        result[index * 2] = digits[input[index] >> 4];
        result[index * 2 + 1] = digits[input[index] & 0x0f];
    }
    return result;
}

inline std::string randomToken() {
    std::array<unsigned char, 32> bytes{};
    const SensitiveBufferGuard cleanseBytes(bytes);
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
        throw std::runtime_error("failed to generate secure token");
    }
    return hexEncode(bytes.data(), bytes.size());
}

inline std::string tokenHash(std::string_view token) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    const SensitiveBufferGuard cleanseDigest(digest);
    unsigned int digestSize = 0;
    if (EVP_Digest(token.data(), token.size(), digest.data(), &digestSize, EVP_sha256(), nullptr) !=
        1) {
        throw std::runtime_error("failed to hash secure token");
    }
    return hexEncode(digest.data(), digestSize);
}

inline bool tokenHashMatches(std::string_view token, std::string_view expectedHash) {
    const auto actualHash = tokenHash(token);
    return actualHash.size() == expectedHash.size() &&
           CRYPTO_memcmp(actualHash.data(), expectedHash.data(), actualHash.size()) == 0;
}

} // namespace service::utils
