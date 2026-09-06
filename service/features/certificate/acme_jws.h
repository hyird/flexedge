#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

#include "service/features/certificate/acme_types.h"

namespace service::certificate_issuance::detail {

using BioPtr = std::unique_ptr<BIO, decltype(&BIO_free)>;
using BnPtr = std::unique_ptr<BIGNUM, decltype(&BN_free)>;
using EvpMdContextPtr = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
using EvpPkeyContextPtr = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
using EvpPkeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;

inline std::string base64Url(std::span<const unsigned char> input) {
    if (input.empty()) {
        return {};
    }
    if (input.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw AcmeError("ACME 编码内容过大", true);
    }
    std::string output(4 * ((input.size() + 2) / 3), '\0');
    const auto size = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(output.data()), input.data(),
                                      static_cast<int>(input.size()));
    if (size < 0) {
        throw AcmeError("ACME Base64URL 编码失败", true);
    }
    output.resize(static_cast<std::size_t>(size));
    std::replace(output.begin(), output.end(), '+', '-');
    std::replace(output.begin(), output.end(), '/', '_');
    while (!output.empty() && output.back() == '=') {
        output.pop_back();
    }
    return output;
}

inline std::string base64Url(std::string_view input) {
    return base64Url(std::span<const unsigned char>(
        reinterpret_cast<const unsigned char*>(input.data()), input.size()));
}

inline std::vector<unsigned char> base64UrlDecode(std::string_view input) {
    std::string encoded(input);
    std::replace(encoded.begin(), encoded.end(), '-', '+');
    std::replace(encoded.begin(), encoded.end(), '_', '/');
    const auto padding = (4 - encoded.size() % 4) % 4;
    encoded.append(padding, '=');
    if (encoded.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw AcmeError("EAB HMAC Key 过大", true);
    }
    std::vector<unsigned char> output((encoded.size() / 4) * 3);
    const auto size =
        EVP_DecodeBlock(output.data(), reinterpret_cast<const unsigned char*>(encoded.data()),
                        static_cast<int>(encoded.size()));
    if (size < 0 || static_cast<std::size_t>(size) < padding) {
        throw AcmeError("EAB HMAC Key 格式无效", true);
    }
    output.resize(static_cast<std::size_t>(size) - padding);
    return output;
}

inline std::string hmacSha256(std::string_view encodedKey, std::string_view input) {
    const auto key = base64UrlDecode(encodedKey);
    std::array<unsigned char, EVP_MAX_MD_SIZE> result{};
    unsigned int size = 0;
    if (HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
             reinterpret_cast<const unsigned char*>(input.data()), input.size(), result.data(),
             &size) == nullptr) {
        throw AcmeError("EAB HMAC 签名失败", true);
    }
    return base64Url(std::span<const unsigned char>(result.data(), size));
}

inline std::array<unsigned char, 32> sha256(std::string_view input) {
    std::array<unsigned char, 32> result{};
    unsigned int size = 0;
    if (EVP_Digest(input.data(), input.size(), result.data(), &size, EVP_sha256(), nullptr) != 1 ||
        size != result.size()) {
        throw AcmeError("SHA-256 计算失败", true);
    }
    return result;
}

inline EvpPkeyPtr generateRsaKey() {
    EvpPkeyContextPtr context(EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free);
    if (!context || EVP_PKEY_keygen_init(context.get()) <= 0 ||
        EVP_PKEY_CTX_set_rsa_keygen_bits(context.get(), 2048) <= 0) {
        throw AcmeError("RSA 密钥生成初始化失败", true);
    }
    EVP_PKEY* raw = nullptr;
    if (EVP_PKEY_keygen(context.get(), &raw) <= 0 || raw == nullptr) {
        throw AcmeError("RSA 密钥生成失败", true);
    }
    return EvpPkeyPtr(raw, EVP_PKEY_free);
}

inline std::string privateKeyPem(EVP_PKEY* key) {
    BioPtr bio(BIO_new(BIO_s_mem()), BIO_free);
    if (!bio ||
        PEM_write_bio_PrivateKey(bio.get(), key, nullptr, nullptr, 0, nullptr, nullptr) != 1) {
        throw AcmeError("私钥序列化失败", true);
    }
    BUF_MEM* buffer = nullptr;
    BIO_get_mem_ptr(bio.get(), &buffer);
    if (buffer == nullptr) {
        throw AcmeError("私钥序列化结果为空", true);
    }
    return std::string(buffer->data, buffer->length);
}

inline EvpPkeyPtr loadPrivateKey(std::string_view pem) {
    BioPtr bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())), BIO_free);
    if (!bio) {
        throw AcmeError("私钥读取失败", true);
    }
    auto* raw = PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr);
    if (raw == nullptr) {
        throw AcmeError("数据库中的私钥格式无效", true);
    }
    return EvpPkeyPtr(raw, EVP_PKEY_free);
}

inline std::string bnBase64Url(const BIGNUM* value) {
    const auto size = BN_num_bytes(value);
    if (size <= 0) {
        throw AcmeError("RSA 公钥参数无效", true);
    }
    std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
    if (BN_bn2bin(value, bytes.data()) != size) {
        throw AcmeError("RSA 公钥参数读取失败", true);
    }
    return base64Url(bytes);
}

struct Jwk final {
    std::string exponent;
    std::string modulus;
};

inline Jwk jwk(EVP_PKEY* key) {
    BIGNUM* modulusRaw = nullptr;
    BIGNUM* exponentRaw = nullptr;
    if (EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_RSA_N, &modulusRaw) != 1 ||
        EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_RSA_E, &exponentRaw) != 1) {
        BN_free(modulusRaw);
        BN_free(exponentRaw);
        throw AcmeError("RSA 公钥参数读取失败", true);
    }
    BnPtr modulus(modulusRaw, BN_free);
    BnPtr exponent(exponentRaw, BN_free);
    return Jwk{bnBase64Url(exponent.get()), bnBase64Url(modulus.get())};
}

inline std::string jwkThumbprint(EVP_PKEY* key) {
    const auto value = jwk(key);
    const auto canonical =
        "{\"e\":\"" + value.exponent + "\",\"kty\":\"RSA\",\"n\":\"" + value.modulus + "\"}";
    const auto digest = sha256(canonical);
    return base64Url(digest);
}

inline std::string sign(EVP_PKEY* key, std::string_view input) {
    EvpMdContextPtr context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!context || EVP_DigestSignInit(context.get(), nullptr, EVP_sha256(), nullptr, key) != 1 ||
        EVP_DigestSignUpdate(context.get(), input.data(), input.size()) != 1) {
        throw AcmeError("ACME JWS 签名初始化失败", true);
    }
    std::size_t size = 0;
    if (EVP_DigestSignFinal(context.get(), nullptr, &size) != 1 || size == 0) {
        throw AcmeError("ACME JWS 签名长度读取失败", true);
    }
    std::vector<unsigned char> signature(size);
    if (EVP_DigestSignFinal(context.get(), signature.data(), &size) != 1) {
        throw AcmeError("ACME JWS 签名失败", true);
    }
    signature.resize(size);
    return base64Url(signature);
}

} // namespace service::certificate_issuance::detail
