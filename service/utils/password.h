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

#include "common/hex.h"
#include "service/utils/sensitive_string.h"

namespace service::password_detail {

inline std::vector<unsigned char> hex_decode(std::string_view value) {
    return flexedge::crypto::hex_decode(value).value_or(std::vector<unsigned char>{});
}

inline bool pbkdf2(std::string_view plain, const unsigned char* salt, std::size_t salt_size,
    int iterations, unsigned char* out, std::size_t out_size) {
    return PKCS5_PBKDF2_HMAC(plain.data(), static_cast<int>(plain.size()), salt,
               static_cast<int>(salt_size), iterations, EVP_sha256(),
               static_cast<int>(out_size), out) == 1;
}

}  // namespace service::password_detail

namespace service::utils {

inline constexpr std::string_view dummy_password_hash{
    "pbkdf2_sha256$210000$00000000000000000000000000000000$"
    "0000000000000000000000000000000000000000000000000000000000000000"};

inline std::string hash_password(std::string_view plain) {
    constexpr int iterations = 210000;
    constexpr std::size_t salt_bytes = 16;
    constexpr std::size_t key_bytes = 32;

    std::array<unsigned char, salt_bytes> salt{};
    const sensitive_buffer_guard cleanse_salt(salt);
    if (RAND_bytes(salt.data(), static_cast<int>(salt.size())) != 1) {
        throw std::runtime_error("RAND_bytes failed");
    }

    std::array<unsigned char, key_bytes> key{};
    const sensitive_buffer_guard cleanse_key(key);
    if (!password_detail::pbkdf2(plain, salt.data(), salt.size(), iterations, key.data(),
            key.size())) {
        throw std::runtime_error("PKCS5_PBKDF2_HMAC failed");
    }

    return "pbkdf2_sha256$" + std::to_string(iterations) + "$" +
           flexedge::crypto::hex_encode(salt) + "$" +
           flexedge::crypto::hex_encode(key);
}

inline bool compare_password(std::string_view plain, std::string_view hash) {
    constexpr std::string_view prefix = "pbkdf2_sha256$";
    if (!hash.starts_with(prefix)) {
        return false;
    }

    auto rest = hash.substr(prefix.size());
    const auto first = rest.find('$');
    if (first == std::string_view::npos) {
        return false;
    }
    const auto second = rest.find('$', first + 1);
    if (second == std::string_view::npos) {
        return false;
    }

    int iterations = 0;
    const auto iteration_text = rest.substr(0, first);
    const auto [ptr, ec] = std::from_chars(iteration_text.data(),
        iteration_text.data() + iteration_text.size(), iterations);
    if (ec != std::errc{} || ptr != iteration_text.data() + iteration_text.size() ||
        iterations <= 0) {
        return false;
    }

    auto salt = password_detail::hex_decode(rest.substr(first + 1, second - first - 1));
    const sensitive_buffer_guard cleanse_salt(salt);
    auto expected = password_detail::hex_decode(rest.substr(second + 1));
    const sensitive_buffer_guard cleanse_expected(expected);
    if (salt.empty() || expected.empty()) {
        return false;
    }

    std::vector<unsigned char> actual(expected.size());
    const sensitive_buffer_guard cleanse_actual(actual);
    if (!password_detail::pbkdf2(plain, salt.data(), salt.size(), iterations, actual.data(),
            actual.size())) {
        return false;
    }
    return CRYPTO_memcmp(actual.data(), expected.data(), expected.size()) == 0;
}

}  // namespace service::utils
