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

#include "service/utils/sensitive_string.h"

namespace service::utils {
namespace secret_detail {

inline constexpr std::size_t kKeySize = 32;
inline constexpr std::size_t kNonceSize = 12;
inline constexpr std::size_t kTagSize = 16;

inline unsigned char decodeNibble(char value) {
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

inline std::vector<unsigned char> decodeHex(std::string_view value) {
    if (value.size() % 2 != 0) {
        throw std::runtime_error("secret envelope contains invalid hexadecimal data");
    }
    std::vector<unsigned char> result(value.size() / 2);
    for (std::size_t index = 0; index < result.size(); ++index) {
        result[index] = static_cast<unsigned char>((decodeNibble(value[index * 2]) << 4) |
                                                   decodeNibble(value[index * 2 + 1]));
    }
    return result;
}

inline std::string encodeHex(const unsigned char* value, std::size_t size) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result(size * 2, '\0');
    for (std::size_t index = 0; index < size; ++index) {
        result[index * 2] = digits[value[index] >> 4];
        result[index * 2 + 1] = digits[value[index] & 0x0f];
    }
    return result;
}

inline int checkedSize(std::size_t size) {
    if (size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("secret value is too large");
    }
    return static_cast<int>(size);
}

using CipherContext = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;

inline std::string sealWithKey(std::string_view plaintext,
                               const std::array<unsigned char, kKeySize>& key) {
    std::array<unsigned char, kNonceSize> nonce{};
    std::array<unsigned char, kTagSize> tag{};
    if (RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1) {
        throw std::runtime_error("failed to generate a secret nonce");
    }

    CipherContext context(EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free);
    if (!context ||
        EVP_EncryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(nonce.size()),
                            nullptr) != 1 ||
        EVP_EncryptInit_ex(context.get(), nullptr, nullptr, key.data(), nonce.data()) != 1) {
        throw std::runtime_error("failed to initialize secret encryption");
    }

    std::vector<unsigned char> ciphertext(plaintext.size() + EVP_MAX_BLOCK_LENGTH);
    int written = 0;
    int finalSize = 0;
    if (EVP_EncryptUpdate(context.get(), ciphertext.data(), &written,
                          reinterpret_cast<const unsigned char*>(plaintext.data()),
                          checkedSize(plaintext.size())) != 1 ||
        EVP_EncryptFinal_ex(context.get(), ciphertext.data() + written, &finalSize) != 1 ||
        EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_GET_TAG, static_cast<int>(tag.size()),
                            tag.data()) != 1) {
        throw std::runtime_error("failed to encrypt secret");
    }
    ciphertext.resize(static_cast<std::size_t>(written) + static_cast<std::size_t>(finalSize));

    return "v1." + encodeHex(nonce.data(), nonce.size()) + "." + encodeHex(tag.data(), tag.size()) +
           "." + encodeHex(ciphertext.data(), ciphertext.size());
}

inline std::string openWithKey(std::string_view envelope,
                               const std::array<unsigned char, kKeySize>& key) {
    if (!envelope.starts_with("v1.")) {
        throw std::runtime_error("unsupported secret envelope version");
    }
    const auto nonceEnd = envelope.find('.', 3);
    const auto tagEnd = nonceEnd == std::string_view::npos ? std::string_view::npos
                                                           : envelope.find('.', nonceEnd + 1);
    if (nonceEnd == std::string_view::npos || tagEnd == std::string_view::npos) {
        throw std::runtime_error("malformed secret envelope");
    }

    const auto nonce = decodeHex(envelope.substr(3, nonceEnd - 3));
    const auto tag = decodeHex(envelope.substr(nonceEnd + 1, tagEnd - nonceEnd - 1));
    const auto ciphertext = decodeHex(envelope.substr(tagEnd + 1));
    if (nonce.size() != kNonceSize || tag.size() != kTagSize) {
        throw std::runtime_error("malformed secret envelope");
    }

    CipherContext context(EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free);
    if (!context ||
        EVP_DecryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(nonce.size()),
                            nullptr) != 1 ||
        EVP_DecryptInit_ex(context.get(), nullptr, nullptr, key.data(), nonce.data()) != 1) {
        throw std::runtime_error("failed to initialize secret decryption");
    }

    std::vector<unsigned char> plaintext(ciphertext.size() + EVP_MAX_BLOCK_LENGTH);
    const SensitiveBufferGuard cleansePlaintext(plaintext);
    int written = 0;
    int finalSize = 0;
    if (EVP_DecryptUpdate(context.get(), plaintext.data(), &written, ciphertext.data(),
                          checkedSize(ciphertext.size())) != 1 ||
        EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_TAG, static_cast<int>(tag.size()),
                            const_cast<unsigned char*>(tag.data())) != 1 ||
        EVP_DecryptFinal_ex(context.get(), plaintext.data() + written, &finalSize) != 1) {
        throw std::runtime_error("secret envelope authentication failed");
    }
    plaintext.resize(static_cast<std::size_t>(written) + static_cast<std::size_t>(finalSize));
    return {reinterpret_cast<const char*>(plaintext.data()), plaintext.size()};
}

inline std::array<unsigned char, kKeySize>& configuredKey() {
    static std::array<unsigned char, kKeySize> key{};
    return key;
}

inline bool& isConfigured() {
    static bool configured = false;
    return configured;
}

} // namespace secret_detail

inline void configureSecretKey(std::string_view encodedKey) {
    auto decoded = secret_detail::decodeHex(encodedKey);
    const SensitiveBufferGuard cleanseDecoded(decoded);
    if (decoded.size() != secret_detail::kKeySize) {
        throw std::runtime_error(
            "SECRET_MASTER_KEY must contain exactly 64 hexadecimal characters");
    }
    auto& key = secret_detail::configuredKey();
    if (secret_detail::isConfigured() &&
        CRYPTO_memcmp(key.data(), decoded.data(), key.size()) != 0) {
        throw std::runtime_error("SECRET_MASTER_KEY cannot change after startup");
    }
    std::copy(decoded.begin(), decoded.end(), key.begin());
    secret_detail::isConfigured() = true;
}

inline std::string sealSecret(std::string_view plaintext) {
    if (!secret_detail::isConfigured()) {
        throw std::runtime_error("SECRET_MASTER_KEY is not configured");
    }
    return secret_detail::sealWithKey(plaintext, secret_detail::configuredKey());
}

inline std::string openSecret(std::string_view envelope) {
    if (!secret_detail::isConfigured()) {
        throw std::runtime_error("SECRET_MASTER_KEY is not configured");
    }
    return secret_detail::openWithKey(envelope, secret_detail::configuredKey());
}

} // namespace service::utils
