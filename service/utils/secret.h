#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include "common/hex.h"
#include "service/utils/sensitive_string.h"

namespace service::utils {
namespace secret_detail {

inline constexpr std::size_t key_size = 32;
inline constexpr std::size_t nonce_size = 12;
inline constexpr std::size_t tag_size = 16;

inline unsigned char decode_nibble(char value) {
    if (value >= '0' && value <= '9') {
        return static_cast<unsigned char>(value - '0');
    }
    if (value >= 'a' && value <= 'f') {
        return static_cast<unsigned char>(value - 'a' + 10);
    }
    if (value >= 'A' && value <= 'F') {
        return static_cast<unsigned char>(value - 'A' + 10);
    }
    throw std::runtime_error("secret envelope contains invalid hexadecimal data");
}

inline std::vector<unsigned char> decode_hex(std::string_view value) {
    if (value.size() % 2 != 0) {
        throw std::runtime_error("secret envelope contains invalid hexadecimal data");
    }
    std::vector<unsigned char> result(value.size() / 2);
    for (std::size_t index = 0; index < result.size(); ++index) {
        result[index] = static_cast<unsigned char>((decode_nibble(value[index * 2]) << 4) |
                                                   decode_nibble(value[index * 2 + 1]));
    }
    return result;
}

inline int checked_size(std::size_t size) {
    if (size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("secret value is too large");
    }
    return static_cast<int>(size);
}

using cipher_context = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;

inline std::string seal_with_key(std::string_view plaintext,
    const std::array<unsigned char, key_size>& key) {
    std::array<unsigned char, nonce_size> nonce{};
    std::array<unsigned char, tag_size> tag{};
    if (RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1) {
        throw std::runtime_error("failed to generate a secret nonce");
    }

    cipher_context context(EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free);
    if (!context ||
        EVP_EncryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(nonce.size()),
            nullptr) != 1 ||
        EVP_EncryptInit_ex(context.get(), nullptr, nullptr, key.data(), nonce.data()) != 1) {
        throw std::runtime_error("failed to initialize secret encryption");
    }

    std::vector<unsigned char> ciphertext(plaintext.size() + EVP_MAX_BLOCK_LENGTH);
    int written = 0;
    int final_size = 0;
    if (EVP_EncryptUpdate(context.get(), ciphertext.data(), &written,
            reinterpret_cast<const unsigned char*>(plaintext.data()),
            checked_size(plaintext.size())) != 1 ||
        EVP_EncryptFinal_ex(context.get(), ciphertext.data() + written, &final_size) != 1 ||
        EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_GET_TAG, static_cast<int>(tag.size()),
            tag.data()) != 1) {
        throw std::runtime_error("failed to encrypt secret");
    }
    ciphertext.resize(static_cast<std::size_t>(written) + static_cast<std::size_t>(final_size));

    return "v1." + flexedge::crypto::hex_encode(nonce) + "." + flexedge::crypto::hex_encode(tag) +
           "." + flexedge::crypto::hex_encode(ciphertext);
}

inline std::string open_with_key(std::string_view envelope,
    const std::array<unsigned char, key_size>& key) {
    if (!envelope.starts_with("v1.")) {
        throw std::runtime_error("unsupported secret envelope version");
    }
    const auto nonce_end = envelope.find('.', 3);
    const auto tag_end = nonce_end == std::string_view::npos ? std::string_view::npos
                                                             : envelope.find('.', nonce_end + 1);
    if (nonce_end == std::string_view::npos || tag_end == std::string_view::npos) {
        throw std::runtime_error("malformed secret envelope");
    }

    const auto nonce = decode_hex(envelope.substr(3, nonce_end - 3));
    const auto tag = decode_hex(envelope.substr(nonce_end + 1, tag_end - nonce_end - 1));
    const auto ciphertext = decode_hex(envelope.substr(tag_end + 1));
    if (nonce.size() != nonce_size || tag.size() != tag_size) {
        throw std::runtime_error("malformed secret envelope");
    }

    cipher_context context(EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free);
    if (!context ||
        EVP_DecryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(nonce.size()),
            nullptr) != 1 ||
        EVP_DecryptInit_ex(context.get(), nullptr, nullptr, key.data(), nonce.data()) != 1) {
        throw std::runtime_error("failed to initialize secret decryption");
    }

    std::vector<unsigned char> plaintext(ciphertext.size() + EVP_MAX_BLOCK_LENGTH);
    const sensitive_buffer_guard cleanse_plaintext(plaintext);
    int written = 0;
    int final_size = 0;
    if (EVP_DecryptUpdate(context.get(), plaintext.data(), &written, ciphertext.data(),
            checked_size(ciphertext.size())) != 1 ||
        EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_TAG, static_cast<int>(tag.size()),
            const_cast<unsigned char*>(tag.data())) != 1 ||
        EVP_DecryptFinal_ex(context.get(), plaintext.data() + written, &final_size) != 1) {
        throw std::runtime_error("secret envelope authentication failed");
    }
    plaintext.resize(static_cast<std::size_t>(written) + static_cast<std::size_t>(final_size));
    return {reinterpret_cast<const char*>(plaintext.data()), plaintext.size()};
}

inline std::array<unsigned char, key_size>& configured_key() {
    static std::array<unsigned char, key_size> key{};
    return key;
}

inline bool& is_configured() {
    static bool configured = false;
    return configured;
}

}  // namespace secret_detail

inline void configure_secret_key(std::string_view encoded_key) {
    auto decoded = secret_detail::decode_hex(encoded_key);
    const sensitive_buffer_guard cleanse_decoded(decoded);
    if (decoded.size() != secret_detail::key_size) {
        throw std::runtime_error(
            "SECRET_MASTER_KEY must contain exactly 64 hexadecimal characters");
    }
    auto& key = secret_detail::configured_key();
    if (secret_detail::is_configured() &&
        CRYPTO_memcmp(key.data(), decoded.data(), key.size()) != 0) {
        throw std::runtime_error("SECRET_MASTER_KEY cannot change after startup");
    }
    std::copy(decoded.begin(), decoded.end(), key.begin());
    secret_detail::is_configured() = true;
}

inline std::string seal_secret(std::string_view plaintext) {
    if (!secret_detail::is_configured()) {
        throw std::runtime_error("SECRET_MASTER_KEY is not configured");
    }
    return secret_detail::seal_with_key(plaintext, secret_detail::configured_key());
}

inline std::string open_secret(std::string_view envelope) {
    if (!secret_detail::is_configured()) {
        throw std::runtime_error("SECRET_MASTER_KEY is not configured");
    }
    return secret_detail::open_with_key(envelope, secret_detail::configured_key());
}

}  // namespace service::utils
