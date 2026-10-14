#pragma once

#include <array>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include "common/hex.h"
#include "service/utils/sensitive_string.h"

namespace service::utils {

inline std::string random_token() {
    std::array<unsigned char, 32> bytes{};
    const sensitive_buffer_guard cleanse_bytes(bytes);
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
        throw std::runtime_error("failed to generate secure token");
    }
    return flexedge::crypto::hex_encode(bytes);
}

inline std::string token_hash(std::string_view token) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    const sensitive_buffer_guard cleanse_digest(digest);
    unsigned int digest_size = 0;
    if (EVP_Digest(token.data(), token.size(), digest.data(), &digest_size, EVP_sha256(), nullptr) !=
        1) {
        throw std::runtime_error("failed to hash secure token");
    }
    return flexedge::crypto::hex_encode({digest.data(), digest_size});
}

inline bool token_hash_matches(std::string_view token, std::string_view expected_hash) {
    const auto actual_hash = token_hash(token);
    return actual_hash.size() == expected_hash.size() &&
           CRYPTO_memcmp(actual_hash.data(), expected_hash.data(), actual_hash.size()) == 0;
}

}  // namespace service::utils
