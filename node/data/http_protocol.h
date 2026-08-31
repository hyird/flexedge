#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/tls1.h>

namespace flexedge::node {

enum class HttpWireProtocol { kHttp1, kHttp2 };

inline HttpWireProtocol negotiatedHttpProtocol(const SSL* ssl) noexcept {
    const unsigned char* selected = nullptr;
    unsigned int size = 0;
    SSL_get0_alpn_selected(ssl, &selected, &size);
    return size == 2 && std::string_view(reinterpret_cast<const char*>(selected), size) == "h2"
               ? HttpWireProtocol::kHttp2
               : HttpWireProtocol::kHttp1;
}

namespace detail {

inline int tlsFingerprintExIndex() noexcept {
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

inline void appendNumberList(std::string& result, const std::vector<std::uint16_t>& values) {
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index > 0) {
            result.push_back('-');
        }
        result.append(std::to_string(values[index]));
    }
}

inline std::string md5Hex(std::string_view value) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digestSize{};
    if (EVP_Digest(value.data(), value.size(), digest.data(), &digestSize, EVP_md5(), nullptr) !=
            1 ||
        digestSize != 16) {
        return {};
    }
    std::array<char, 33> output{};
    for (unsigned int index = 0; index < digestSize; ++index) {
        std::snprintf(output.data() + index * 2, 3, "%02x", digest[index]);
    }
    return std::string(output.data(), 32);
}

inline std::vector<std::uint16_t> cipherSuites(SSL* ssl) {
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

inline std::vector<std::uint16_t> extensionTypes(SSL* ssl) {
    int* present = nullptr;
    std::size_t presentSize{};
    if (SSL_client_hello_get1_extensions_present(ssl, &present, &presentSize) != 1) {
        return {};
    }
    std::vector<std::uint16_t> ordered;
    if (presentSize > 0) {
        ordered.resize(presentSize);
        auto orderedSize = presentSize;
        if (SSL_client_hello_get_extension_order(ssl, ordered.data(), &orderedSize) == 1) {
            ordered.resize(orderedSize);
        } else {
            ordered.clear();
            ordered.reserve(presentSize);
            for (std::size_t index = 0; index < presentSize; ++index) {
                ordered.push_back(static_cast<std::uint16_t>(present[index]));
            }
        }
    }
    OPENSSL_free(present);
    std::erase_if(ordered, grease);
    return ordered;
}

inline std::vector<std::uint16_t> supportedGroups(SSL* ssl) {
    const unsigned char* data = nullptr;
    std::size_t size{};
    if (SSL_client_hello_get0_ext(ssl, TLSEXT_TYPE_supported_groups, &data, &size) != 1 ||
        data == nullptr || size < 2) {
        return {};
    }
    const auto listSize = (std::min)(static_cast<std::size_t>(u16(data)), size - 2);
    std::vector<std::uint16_t> values;
    values.reserve(listSize / 2);
    for (std::size_t offset = 2; offset + 1 < 2 + listSize; offset += 2) {
        const auto value = u16(data + offset);
        if (!grease(value)) {
            values.push_back(value);
        }
    }
    return values;
}

inline std::vector<std::uint16_t> pointFormats(SSL* ssl) {
    const unsigned char* data = nullptr;
    std::size_t size{};
    if (SSL_client_hello_get0_ext(ssl, TLSEXT_TYPE_ec_point_formats, &data, &size) != 1 ||
        data == nullptr || size < 1) {
        return {};
    }
    const auto listSize = (std::min)(static_cast<std::size_t>(data[0]), size - 1);
    std::vector<std::uint16_t> values;
    values.reserve(listSize);
    for (std::size_t offset = 1; offset < 1 + listSize; ++offset) {
        values.push_back(data[offset]);
    }
    return values;
}

inline std::string ja3Fingerprint(SSL* ssl) {
    std::string value = std::to_string(SSL_client_hello_get0_legacy_version(ssl));
    value.push_back(',');
    appendNumberList(value, cipherSuites(ssl));
    value.push_back(',');
    appendNumberList(value, extensionTypes(ssl));
    value.push_back(',');
    appendNumberList(value, supportedGroups(ssl));
    value.push_back(',');
    appendNumberList(value, pointFormats(ssl));
    const auto digest = md5Hex(value);
    return digest.empty() ? std::string{} : "ja3:" + digest;
}

inline int tlsFingerprintCallback(SSL* ssl, int*, void*) noexcept {
    try {
        const auto index = tlsFingerprintExIndex();
        auto* output = index < 0 ? nullptr : static_cast<std::string*>(SSL_get_ex_data(ssl, index));
        if (output != nullptr) {
            *output = ja3Fingerprint(ssl);
        }
    } catch (...) {
    }
    return SSL_CLIENT_HELLO_SUCCESS;
}

} // namespace detail

inline void captureTlsClientFingerprint(SSL* ssl, std::string& output) noexcept {
    const auto index = detail::tlsFingerprintExIndex();
    if (index < 0) {
        return;
    }
    SSL_set_ex_data(ssl, index, &output);
    SSL_CTX_set_client_hello_cb(SSL_get_SSL_CTX(ssl), &detail::tlsFingerprintCallback, nullptr);
}

inline void releaseTlsClientFingerprint(SSL* ssl) noexcept {
    const auto index = detail::tlsFingerprintExIndex();
    if (index >= 0) {
        SSL_set_ex_data(ssl, index, nullptr);
    }
}

} // namespace flexedge::node
