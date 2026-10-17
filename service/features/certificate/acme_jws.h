#pragma once

#include <array>
#include <cstddef>
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
#include "service/utils/base64.h"

namespace service::certificate_issuance::detail {

using bio_ptr = std::unique_ptr<BIO, decltype(&BIO_free)>;
using bn_ptr = std::unique_ptr<BIGNUM, decltype(&BN_free)>;
using evp_md_context_ptr = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
using evp_pkey_context_ptr = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
using evp_pkey_ptr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;

inline std::string base64_url(std::string_view input) {
    return service::utils::base64_url_encode(input);
}

inline std::string base64_url(std::span<const unsigned char> input) {
    return service::utils::base64_url_encode(
        std::string_view(reinterpret_cast<const char*>(input.data()), input.size()));
}

inline std::string base64_url_decode(std::string_view input) {
    auto decoded = service::utils::base64_url_decode(input);
    if (!decoded || decoded->empty()) {
        throw acme_error("EAB HMAC Key 格式无效", true);
    }
    return std::move(*decoded);
}

inline std::string hmac_sha256(std::string_view encoded_key, std::string_view input) {
    const auto key = base64_url_decode(encoded_key);
    std::array<unsigned char, EVP_MAX_MD_SIZE> result{};
    unsigned int size = 0;
    if (HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
            reinterpret_cast<const unsigned char*>(input.data()), input.size(), result.data(),
            &size) == nullptr) {
        throw acme_error("EAB HMAC 签名失败", true);
    }
    return base64_url(std::span<const unsigned char>(result.data(), size));
}

inline std::array<unsigned char, 32> sha256(std::string_view input) {
    std::array<unsigned char, 32> result{};
    unsigned int size = 0;
    if (EVP_Digest(input.data(), input.size(), result.data(), &size, EVP_sha256(), nullptr) != 1 ||
        size != result.size()) {
        throw acme_error("SHA-256 计算失败", true);
    }
    return result;
}

inline evp_pkey_ptr generate_rsa_key() {
    evp_pkey_context_ptr context(EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free);
    if (!context || EVP_PKEY_keygen_init(context.get()) <= 0 ||
        EVP_PKEY_CTX_set_rsa_keygen_bits(context.get(), 2048) <= 0) {
        throw acme_error("RSA 密钥生成初始化失败", true);
    }
    EVP_PKEY* raw = nullptr;
    if (EVP_PKEY_keygen(context.get(), &raw) <= 0 || raw == nullptr) {
        throw acme_error("RSA 密钥生成失败", true);
    }
    return evp_pkey_ptr(raw, EVP_PKEY_free);
}

inline std::string private_key_pem(EVP_PKEY* key) {
    bio_ptr bio(BIO_new(BIO_s_mem()), BIO_free);
    if (!bio ||
        PEM_write_bio_PrivateKey(bio.get(), key, nullptr, nullptr, 0, nullptr, nullptr) != 1) {
        throw acme_error("私钥序列化失败", true);
    }
    BUF_MEM* buffer = nullptr;
    BIO_get_mem_ptr(bio.get(), &buffer);
    if (buffer == nullptr) {
        throw acme_error("私钥序列化结果为空", true);
    }
    return std::string(buffer->data, buffer->length);
}

inline evp_pkey_ptr load_private_key(std::string_view pem) {
    bio_ptr bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())), BIO_free);
    if (!bio) {
        throw acme_error("私钥读取失败", true);
    }
    auto* raw = PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr);
    if (raw == nullptr) {
        throw acme_error("数据库中的私钥格式无效", true);
    }
    return evp_pkey_ptr(raw, EVP_PKEY_free);
}

inline std::string bn_base64_url(const BIGNUM* value) {
    const auto size = BN_num_bytes(value);
    if (size <= 0) {
        throw acme_error("RSA 公钥参数无效", true);
    }
    std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
    if (BN_bn2bin(value, bytes.data()) != size) {
        throw acme_error("RSA 公钥参数读取失败", true);
    }
    return base64_url(bytes);
}

struct jwk final {
    std::string exponent_;
    std::string modulus_;
};

inline jwk get_jwk(EVP_PKEY* key) {
    BIGNUM* modulus_raw = nullptr;
    BIGNUM* exponent_raw = nullptr;
    if (EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_RSA_N, &modulus_raw) != 1 ||
        EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_RSA_E, &exponent_raw) != 1) {
        BN_free(modulus_raw);
        BN_free(exponent_raw);
        throw acme_error("RSA 公钥参数读取失败", true);
    }
    bn_ptr modulus(modulus_raw, BN_free);
    bn_ptr exponent(exponent_raw, BN_free);
    return jwk{bn_base64_url(exponent.get()), bn_base64_url(modulus.get())};
}

inline std::string jwk_thumbprint(EVP_PKEY* key) {
    const auto value = get_jwk(key);
    const auto canonical =
        "{\"e\":\"" + value.exponent_ + "\",\"kty\":\"RSA\",\"n\":\"" + value.modulus_ + "\"}";
    const auto digest = sha256(canonical);
    return base64_url(digest);
}

inline std::string sign(EVP_PKEY* key, std::string_view input) {
    evp_md_context_ptr context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!context || EVP_DigestSignInit(context.get(), nullptr, EVP_sha256(), nullptr, key) != 1 ||
        EVP_DigestSignUpdate(context.get(), input.data(), input.size()) != 1) {
        throw acme_error("ACME JWS 签名初始化失败", true);
    }
    std::size_t size = 0;
    if (EVP_DigestSignFinal(context.get(), nullptr, &size) != 1 || size == 0) {
        throw acme_error("ACME JWS 签名长度读取失败", true);
    }
    std::vector<unsigned char> signature(size);
    if (EVP_DigestSignFinal(context.get(), signature.data(), &size) != 1) {
        throw acme_error("ACME JWS 签名失败", true);
    }
    signature.resize(size);
    return base64_url(signature);
}

}  // namespace service::certificate_issuance::detail
