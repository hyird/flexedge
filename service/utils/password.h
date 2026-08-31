#pragma once

#include <array>
#include <charconv>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include "service/utils/sensitive_string.h"

namespace service::password_detail {

inline std::string hexEncode(const unsigned char* data, std::size_t size) {
    constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        out.push_back(hex[data[i] >> 4]);
        out.push_back(hex[data[i] & 0x0F]);
    }
    return out;
}

inline int hexValue(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

inline std::vector<unsigned char> hexDecode(std::string_view value) {
    if (value.size() % 2 != 0) {
        return {};
    }
    std::vector<unsigned char> out;
    out.reserve(value.size() / 2);
    for (std::size_t i = 0; i < value.size(); i += 2) {
        const int high = hexValue(value[i]);
        const int low = hexValue(value[i + 1]);
        if (high < 0 || low < 0) {
            return {};
        }
        out.push_back(static_cast<unsigned char>((high << 4) | low));
    }
    return out;
}

inline bool pbkdf2(std::string_view plain, const unsigned char* salt, std::size_t saltSize,
                   int iterations, unsigned char* out, std::size_t outSize) {
    return PKCS5_PBKDF2_HMAC(plain.data(), static_cast<int>(plain.size()), salt,
                             static_cast<int>(saltSize), iterations, EVP_sha256(),
                             static_cast<int>(outSize), out) == 1;
}

} // namespace service::password_detail

namespace service::utils {

inline constexpr std::string_view kDummyPasswordHash{
    "pbkdf2_sha256$210000$00000000000000000000000000000000$"
    "0000000000000000000000000000000000000000000000000000000000000000"};

inline std::string hashPassword(std::string_view plain) {
    constexpr int kIterations = 210000;
    constexpr std::size_t kSaltBytes = 16;
    constexpr std::size_t kKeyBytes = 32;

    std::array<unsigned char, kSaltBytes> salt{};
    const SensitiveBufferGuard cleanseSalt(salt);
    if (RAND_bytes(salt.data(), static_cast<int>(salt.size())) != 1) {
        throw std::runtime_error("RAND_bytes failed");
    }

    std::array<unsigned char, kKeyBytes> key{};
    const SensitiveBufferGuard cleanseKey(key);
    if (!password_detail::pbkdf2(plain, salt.data(), salt.size(), kIterations, key.data(),
                                 key.size())) {
        throw std::runtime_error("PKCS5_PBKDF2_HMAC failed");
    }

    return "pbkdf2_sha256$" + std::to_string(kIterations) + "$" +
           password_detail::hexEncode(salt.data(), salt.size()) + "$" +
           password_detail::hexEncode(key.data(), key.size());
}

inline bool comparePassword(std::string_view plain, std::string_view hash) {
    constexpr std::string_view kPrefix = "pbkdf2_sha256$";
    if (!hash.starts_with(kPrefix)) {
        return false;
    }

    auto rest = hash.substr(kPrefix.size());
    const auto first = rest.find('$');
    if (first == std::string_view::npos) {
        return false;
    }
    const auto second = rest.find('$', first + 1);
    if (second == std::string_view::npos) {
        return false;
    }

    int iterations = 0;
    const auto iterationText = rest.substr(0, first);
    const auto [ptr, ec] = std::from_chars(iterationText.data(),
                                           iterationText.data() + iterationText.size(), iterations);
    if (ec != std::errc{} || ptr != iterationText.data() + iterationText.size() ||
        iterations <= 0) {
        return false;
    }

    auto salt = password_detail::hexDecode(rest.substr(first + 1, second - first - 1));
    const SensitiveBufferGuard cleanseSalt(salt);
    auto expected = password_detail::hexDecode(rest.substr(second + 1));
    const SensitiveBufferGuard cleanseExpected(expected);
    if (salt.empty() || expected.empty()) {
        return false;
    }

    std::vector<unsigned char> actual(expected.size());
    const SensitiveBufferGuard cleanseActual(actual);
    if (!password_detail::pbkdf2(plain, salt.data(), salt.size(), iterations, actual.data(),
                                 actual.size())) {
        return false;
    }
    return CRYPTO_memcmp(actual.data(), expected.data(), expected.size()) == 0;
}

} // namespace service::utils
