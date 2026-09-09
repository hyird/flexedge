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
#include "common/hex.h"

namespace service::utils {

inline std::string randomToken() {
    std::array<unsigned char, 32> bytes{};
    const SensitiveBufferGuard cleanseBytes(bytes);
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
        throw std::runtime_error("failed to generate secure token");
    }
    return flexedge::crypto::hexEncode(bytes);
}

inline std::string tokenHash(std::string_view token) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    const SensitiveBufferGuard cleanseDigest(digest);
    unsigned int digestSize = 0;
    if (EVP_Digest(token.data(), token.size(), digest.data(), &digestSize, EVP_sha256(), nullptr) !=
        1) {
        throw std::runtime_error("failed to hash secure token");
    }
    return flexedge::crypto::hexEncode({digest.data(), digestSize});
}

inline bool tokenHashMatches(std::string_view token, std::string_view expectedHash) {
    const auto actualHash = tokenHash(token);
    return actualHash.size() == expectedHash.size() &&
           CRYPTO_memcmp(actualHash.data(), expectedHash.data(), actualHash.size()) == 0;
}

} // namespace service::utils
