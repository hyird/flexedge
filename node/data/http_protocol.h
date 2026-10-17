#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/tls1.h>

#include "common/hex.h"

namespace flexedge::node {

namespace detail {

inline int tls_fingerprint_ex_index() noexcept {
    static const int index = SSL_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
    return index;
}

inline bool grease(std::uint16_t value) noexcept {
    return (value & 0x0f0fU) == 0x0a0aU &&
           static_cast<unsigned char>(value >> 8) == static_cast<unsigned char>(value);
}

inline std::uint16_t u16(const unsigned char* data) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[0]) << 8U) |
                                      static_cast<std::uint16_t>(data[1]));
}

inline void append_number_list(std::string& result, const std::vector<std::uint16_t>& values) {
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index > 0) {
            result.push_back('-');
        }
        result.append(std::to_string(values[index]));
    }
}

inline std::string md5_hex(std::string_view value) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_size{};
    if (EVP_Digest(value.data(), value.size(), digest.data(), &digest_size, EVP_md5(), nullptr) !=
            1 ||
        digest_size != 16) {
        return {};
    }
    return flexedge::crypto::hex_encode({digest.data(), digest_size});
}

inline std::vector<std::uint16_t> cipher_suites(SSL* ssl) {
    const unsigned char* data = nullptr;
    const auto size = SSL_client_hello_get0_ciphers(ssl, &data);
    std::vector<std::uint16_t> values;
    values.reserve(size / 2);
    for (std::size_t offset = 0; data != nullptr && offset + 1 < size; offset += 2) {
        const auto value = u16(data + offset);
        if (!grease(value)) {
            values.push_back(value);
        }
    }
    return values;
}

inline std::vector<std::uint16_t> extension_types(SSL* ssl) {
    int* present = nullptr;
    std::size_t present_size{};
    if (SSL_client_hello_get1_extensions_present(ssl, &present, &present_size) != 1) {
        return {};
    }
    std::vector<std::uint16_t> ordered;
    if (present_size > 0) {
        ordered.resize(present_size);
        auto ordered_size = present_size;
        if (SSL_client_hello_get_extension_order(ssl, ordered.data(), &ordered_size) == 1) {
            ordered.resize(ordered_size);
        } else {
            ordered.clear();
            ordered.reserve(present_size);
            for (std::size_t index = 0; index < present_size; ++index) {
                ordered.push_back(static_cast<std::uint16_t>(present[index]));
            }
        }
    }
    OPENSSL_free(present);
    std::erase_if(ordered, grease);
    return ordered;
}

inline std::vector<std::uint16_t> supported_groups(SSL* ssl) {
    const unsigned char* data = nullptr;
    std::size_t size{};
    if (SSL_client_hello_get0_ext(ssl, TLSEXT_TYPE_supported_groups, &data, &size) != 1 ||
        data == nullptr || size < 2) {
        return {};
    }
    const auto list_size = (std::min)(static_cast<std::size_t>(u16(data)), size - 2);
    std::vector<std::uint16_t> values;
    values.reserve(list_size / 2);
    for (std::size_t offset = 2; offset + 1 < 2 + list_size; offset += 2) {
        const auto value = u16(data + offset);
        if (!grease(value)) {
            values.push_back(value);
        }
    }
    return values;
}

inline std::vector<std::uint16_t> point_formats(SSL* ssl) {
    const unsigned char* data = nullptr;
    std::size_t size{};
    if (SSL_client_hello_get0_ext(ssl, TLSEXT_TYPE_ec_point_formats, &data, &size) != 1 ||
        data == nullptr || size < 1) {
        return {};
    }
    const auto list_size = (std::min)(static_cast<std::size_t>(data[0]), size - 1);
    std::vector<std::uint16_t> values;
    values.reserve(list_size);
    for (std::size_t offset = 1; offset < 1 + list_size; ++offset) {
        values.push_back(data[offset]);
    }
    return values;
}

inline std::string ja3_fingerprint(SSL* ssl) {
    std::string value = std::to_string(SSL_client_hello_get0_legacy_version(ssl));
    value.push_back(',');
    append_number_list(value, cipher_suites(ssl));
    value.push_back(',');
    append_number_list(value, extension_types(ssl));
    value.push_back(',');
    append_number_list(value, supported_groups(ssl));
    value.push_back(',');
    append_number_list(value, point_formats(ssl));
    const auto digest = md5_hex(value);
    return digest.empty() ? std::string{} : "ja3:" + digest;
}

inline int tls_fingerprint_callback(SSL* ssl, int*, void*) noexcept {
    try {
        const auto index = tls_fingerprint_ex_index();
        auto* output = index < 0 ? nullptr : static_cast<std::string*>(SSL_get_ex_data(ssl, index));
        if (output != nullptr) {
            *output = ja3_fingerprint(ssl);
        }
    } catch (...) {
    }
    return SSL_CLIENT_HELLO_SUCCESS;
}

}  // namespace detail

inline void capture_tls_client_fingerprint(SSL* ssl, std::string& output) noexcept {
    const auto index = detail::tls_fingerprint_ex_index();
    if (index < 0) {
        return;
    }
    SSL_set_ex_data(ssl, index, &output);
    SSL_CTX_set_client_hello_cb(SSL_get_SSL_CTX(ssl), &detail::tls_fingerprint_callback, nullptr);
}

inline void release_tls_client_fingerprint(SSL* ssl) noexcept {
    const auto index = detail::tls_fingerprint_ex_index();
    if (index >= 0) {
        SSL_set_ex_data(ssl, index, nullptr);
    }
}

}  // namespace flexedge::node
