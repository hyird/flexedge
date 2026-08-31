#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <ruvia/core/Task.h>
#include <ruvia/core/Timer.h>
#include <ruvia/http/HttpKnownMethod.h>
#include <ruvia/http/HttpStatus.h>
#include <ruvia/web/HttpClientHandle.h>
#include <ruvia/web/Model.h>

#include "service/config/outbound.h"
#include "service/features/certificate/dns_challenge.h"
#include "service/features/certificate/provider_config.h"
#include "service/features/outbound_http/client.h"
#include "service/features/sync_runtime/state.h"
#include "service/utils/secret.h"

namespace service::certificate_issuance {

RUVIA_REQUEST_MODEL(AcmeDirectoryInput,
                    RUVIA_OPTIONAL_FIELD_NAME("newNonce", newNonce, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("newAccount", newAccount, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("newOrder", newOrder, ruvia::String));
RUVIA_REQUEST_MODEL(AcmeProblemInput, RUVIA_OPTIONAL_FIELD(type, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(detail, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(status, ruvia::Int64));
RUVIA_REQUEST_MODEL(AcmeIdentifierInput, RUVIA_OPTIONAL_FIELD(type, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(value, ruvia::String));
RUVIA_REQUEST_MODEL(AcmeChallengeInput, RUVIA_OPTIONAL_FIELD(type, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(url, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(status, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(token, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(error, AcmeProblemInput));
RUVIA_REQUEST_MODEL(AcmeAuthorizationInput, RUVIA_OPTIONAL_FIELD(status, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(identifier, AcmeIdentifierInput),
                    RUVIA_OPTIONAL_FIELD(challenges, ruvia::Array<AcmeChallengeInput>));
RUVIA_REQUEST_MODEL(AcmeOrderInput, RUVIA_OPTIONAL_FIELD(status, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(authorizations, ruvia::Array<ruvia::String>),
                    RUVIA_OPTIONAL_FIELD(finalize, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(certificate, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(error, AcmeProblemInput));

RUVIA_RESPONSE_MODEL(AcmeJwkOutput, RUVIA_REQUIRED_FIELD(e, ruvia::String),
                     RUVIA_REQUIRED_FIELD(kty, ruvia::String),
                     RUVIA_REQUIRED_FIELD(n, ruvia::String));
RUVIA_RESPONSE_MODEL(AcmeProtectedJwkOutput, RUVIA_REQUIRED_FIELD(alg, ruvia::String),
                     RUVIA_REQUIRED_FIELD(jwk, AcmeJwkOutput),
                     RUVIA_REQUIRED_FIELD(nonce, ruvia::String),
                     RUVIA_REQUIRED_FIELD(url, ruvia::String));
RUVIA_RESPONSE_MODEL(AcmeProtectedKidOutput, RUVIA_REQUIRED_FIELD(alg, ruvia::String),
                     RUVIA_REQUIRED_FIELD(kid, ruvia::String),
                     RUVIA_REQUIRED_FIELD(nonce, ruvia::String),
                     RUVIA_REQUIRED_FIELD(url, ruvia::String));
RUVIA_RESPONSE_MODEL(AcmeJwsOutput,
                     RUVIA_REQUIRED_FIELD_NAME("protected", protectedValue, ruvia::String),
                     RUVIA_REQUIRED_FIELD(payload, ruvia::String),
                     RUVIA_REQUIRED_FIELD(signature, ruvia::String));
RUVIA_RESPONSE_MODEL(AcmeEabProtectedOutput, RUVIA_REQUIRED_FIELD(alg, ruvia::String),
                     RUVIA_REQUIRED_FIELD(kid, ruvia::String),
                     RUVIA_REQUIRED_FIELD(url, ruvia::String));
RUVIA_RESPONSE_MODEL(AcmeAccountPayloadOutput,
                     RUVIA_REQUIRED_FIELD_NAME("termsOfServiceAgreed", termsAgreed, ruvia::Bool),
                     RUVIA_OPTIONAL_FIELD(contact, ruvia::Array<ruvia::String>, RUVIA_OMIT_EMPTY),
                     RUVIA_OPTIONAL_FIELD_NAME("externalAccountBinding", externalAccountBinding,
                                               AcmeJwsOutput));
RUVIA_RESPONSE_MODEL(AcmeIdentifierOutput, RUVIA_REQUIRED_FIELD(type, ruvia::String),
                     RUVIA_REQUIRED_FIELD(value, ruvia::String));
RUVIA_RESPONSE_MODEL(AcmeOrderPayloadOutput,
                     RUVIA_REQUIRED_FIELD(identifiers, ruvia::Array<AcmeIdentifierOutput>));
RUVIA_RESPONSE_MODEL(AcmeFinalizePayloadOutput, RUVIA_REQUIRED_FIELD(csr, ruvia::String));
RUVIA_RESPONSE_MODEL(AcmeEmptyPayloadOutput,
                     RUVIA_OPTIONAL_FIELD(unused, ruvia::String, RUVIA_OMIT_EMPTY));

struct AcmeSettings final {
    std::string host;
    std::string directoryTarget;
    std::string directoryUrl;
};

inline std::optional<AcmeSettings> settingsForProvider(std::string_view provider) {
    if (provider == "letsencrypt") {
        return AcmeSettings{
            .host = "acme-v02.api.letsencrypt.org",
            .directoryTarget = "/directory",
            .directoryUrl = "https://acme-v02.api.letsencrypt.org/directory",
        };
    }
    if (provider == "zerossl") {
        return AcmeSettings{
            .host = "acme.zerossl.com",
            .directoryTarget = "/v2/DV90",
            .directoryUrl = "https://acme.zerossl.com/v2/DV90",
        };
    }
    return std::nullopt;
}

class AcmeError final : public std::runtime_error {
  public:
    AcmeError(std::string_view message, bool permanent)
        : std::runtime_error(std::string(message)), permanent_(permanent) {}

    [[nodiscard]] bool permanent() const noexcept { return permanent_; }

  private:
    bool permanent_;
};

struct AcmeAccount final {
    service::utils::SensitiveString privateKeyPem;
    std::string accountUrl;
};

struct IssuedCertificate final {
    service::utils::SensitiveString privateKeyPem;
    std::string certificateChainPem;
    std::string notBefore;
    std::string expiresAt;
    std::string serialNumber;
    std::string fingerprintSha256;
};

namespace detail {

using BioPtr = std::unique_ptr<BIO, decltype(&BIO_free)>;
using BnPtr = std::unique_ptr<BIGNUM, decltype(&BN_free)>;
using EvpMdContextPtr = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
using EvpPkeyContextPtr = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
using EvpPkeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using X509Ptr = std::unique_ptr<X509, decltype(&X509_free)>;
using X509RequestPtr = std::unique_ptr<X509_REQ, decltype(&X509_REQ_free)>;

inline std::string jsonString(auto&& model, std::pmr::memory_resource* resource) {
    const auto value = ruvia::toJson(std::forward<decltype(model)>(model), {.resource = resource});
    return std::string(value.data(), value.size());
}

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

struct CertificateRequest final {
    service::utils::SensitiveString privateKeyPem;
    std::string csr;
};

inline CertificateRequest createCertificateRequest(const std::vector<std::string>& domains) {
    if (domains.empty()) {
        throw AcmeError("证书标识不能为空", true);
    }
    auto key = generateRsaKey();
    X509RequestPtr request(X509_REQ_new(), X509_REQ_free);
    if (!request || X509_REQ_set_version(request.get(), 0L) != 1 ||
        X509_REQ_set_pubkey(request.get(), key.get()) != 1) {
        throw AcmeError("证书 CSR 初始化失败", true);
    }

    auto* subject = X509_REQ_get_subject_name(request.get());
    if (subject == nullptr ||
        X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char*>(domains.front().data()),
                                   static_cast<int>(domains.front().size()), -1, 0) != 1) {
        throw AcmeError("证书 CSR 主体设置失败", true);
    }

    std::string san;
    for (const auto& domain : domains) {
        if (!san.empty()) {
            san.push_back(',');
        }
        san.append("DNS:").append(domain);
    }
    X509V3_CTX extensionContext{};
    X509V3_set_ctx_nodb(&extensionContext);
    X509V3_set_ctx(&extensionContext, nullptr, nullptr, request.get(), nullptr, 0);
    auto* extension =
        X509V3_EXT_conf_nid(nullptr, &extensionContext, NID_subject_alt_name, san.data());
    if (extension == nullptr) {
        throw AcmeError("证书 CSR SAN 设置失败", true);
    }
    auto* extensions = sk_X509_EXTENSION_new_null();
    if (extensions == nullptr || sk_X509_EXTENSION_push(extensions, extension) == 0) {
        X509_EXTENSION_free(extension);
        sk_X509_EXTENSION_free(extensions);
        throw AcmeError("证书 CSR 扩展创建失败", true);
    }
    const auto extensionsAdded = X509_REQ_add_extensions(request.get(), extensions);
    sk_X509_EXTENSION_pop_free(extensions, X509_EXTENSION_free);
    if (extensionsAdded != 1 || X509_REQ_sign(request.get(), key.get(), EVP_sha256()) <= 0) {
        throw AcmeError("证书 CSR 签名失败", true);
    }

    const auto derSize = i2d_X509_REQ(request.get(), nullptr);
    if (derSize <= 0) {
        throw AcmeError("证书 CSR 编码失败", true);
    }
    std::vector<unsigned char> der(static_cast<std::size_t>(derSize));
    auto* output = der.data();
    if (i2d_X509_REQ(request.get(), &output) != derSize) {
        throw AcmeError("证书 CSR 编码失败", true);
    }
    return CertificateRequest{service::utils::SensitiveString(privateKeyPem(key.get())),
                              base64Url(der)};
}

inline std::string formatAsn1Time(const ASN1_TIME* value) {
    std::tm time{};
    if (value == nullptr || ASN1_TIME_to_tm(value, &time) != 1) {
        throw AcmeError("证书有效期解析失败", true);
    }
    std::array<char, 32> output{};
    if (std::strftime(output.data(), output.size(), "%Y-%m-%dT%H:%M:%SZ", &time) == 0) {
        throw AcmeError("证书有效期格式化失败", true);
    }
    return output.data();
}

inline std::string serialNumber(X509* certificate) {
    BnPtr number(ASN1_INTEGER_to_BN(X509_get_serialNumber(certificate), nullptr), BN_free);
    if (!number) {
        throw AcmeError("证书序列号读取失败", true);
    }
    char* raw = BN_bn2hex(number.get());
    if (raw == nullptr) {
        throw AcmeError("证书序列号编码失败", true);
    }
    std::string result(raw);
    OPENSSL_free(raw);
    return result;
}

inline std::string fingerprint(X509* certificate) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int size = 0;
    if (X509_digest(certificate, EVP_sha256(), digest.data(), &size) != 1) {
        throw AcmeError("证书指纹计算失败", true);
    }
    std::ostringstream output;
    output << std::uppercase << std::hex << std::setfill('0');
    for (unsigned int index = 0; index < size; ++index) {
        if (index != 0) {
            output << ':';
        }
        output << std::setw(2) << static_cast<unsigned>(digest[index]);
    }
    return output.str();
}

inline void validateCertificateDomains(X509* certificate,
                                       const std::vector<std::string>& expectedDomains) {
    auto* names = static_cast<GENERAL_NAMES*>(
        X509_get_ext_d2i(certificate, NID_subject_alt_name, nullptr, nullptr));
    if (names == nullptr) {
        throw AcmeError("签发证书缺少 SAN", true);
    }
    std::set<std::string> actual;
    const auto count = sk_GENERAL_NAME_num(names);
    for (int index = 0; index < count; ++index) {
        const auto* name = sk_GENERAL_NAME_value(names, index);
        if (name == nullptr || name->type != GEN_DNS) {
            continue;
        }
        const auto* data = ASN1_STRING_get0_data(name->d.dNSName);
        const auto length = ASN1_STRING_length(name->d.dNSName);
        if (data != nullptr && length > 0) {
            actual.emplace(reinterpret_cast<const char*>(data), static_cast<std::size_t>(length));
        }
    }
    GENERAL_NAMES_free(names);
    const std::set<std::string> expected(expectedDomains.begin(), expectedDomains.end());
    if (actual != expected) {
        throw AcmeError("签发证书的域名集合与申请不一致", true);
    }
}

inline IssuedCertificate inspectCertificate(service::utils::SensitiveString privateKey,
                                            std::string certificateChain,
                                            const std::vector<std::string>& domains) {
    auto key = loadPrivateKey(privateKey.view());
    BioPtr bio(BIO_new_mem_buf(certificateChain.data(), static_cast<int>(certificateChain.size())),
               BIO_free);
    if (!bio) {
        throw AcmeError("证书链读取失败", true);
    }
    X509Ptr certificate(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr), X509_free);
    if (!certificate) {
        throw AcmeError("ACME 返回的证书链格式无效", true);
    }
    if (X509_check_private_key(certificate.get(), key.get()) != 1) {
        throw AcmeError("签发证书与私钥不匹配", true);
    }
    validateCertificateDomains(certificate.get(), domains);
    return IssuedCertificate{
        std::move(privateKey),
        std::move(certificateChain),
        formatAsn1Time(X509_get0_notBefore(certificate.get())),
        formatAsn1Time(X509_get0_notAfter(certificate.get())),
        serialNumber(certificate.get()),
        fingerprint(certificate.get()),
    };
}

struct Directory final {
    std::string newNonce;
    std::string newAccount;
    std::string newOrder;
};

struct Challenge final {
    std::string url;
    std::string token;
};

struct Authorization final {
    std::string status;
    std::string identifier;
    std::optional<Challenge> dnsChallenge;
    std::string error;
};

struct Order final {
    std::string status;
    std::vector<std::string> authorizations;
    std::string finalize;
    std::string certificate;
    std::string error;
};

struct PendingAuthorization final {
    std::string authorizationUrl;
    std::string challengeUrl;
};

struct AuthorizationPlan final {
    std::vector<PendingAuthorization> pending;
    std::vector<DnsChallengeSpec> challengeSpecs;
};

struct OrderSession final {
    Directory directory;
    std::string url;
    Order order;
};

inline std::string optionalString(const std::optional<ruvia::String>& value) {
    return value ? std::string(value->view()) : std::string{};
}

inline std::string problemDetail(const std::optional<AcmeProblemInput>& problem) {
    if (!problem) {
        return {};
    }
    if (const auto& detailValue = problem->get<"detail">(); detailValue) {
        return std::string(detailValue->view());
    }
    if (const auto& typeValue = problem->get<"type">(); typeValue) {
        return std::string(typeValue->view());
    }
    return {};
}

} // namespace detail

class AcmeClient final {
  public:
    explicit AcmeClient(AcmeSettings settings) : settings_(std::move(settings)) {}

    [[nodiscard]] const AcmeSettings& settings() const noexcept { return settings_; }

    [[nodiscard]] service::utils::SensitiveString generateAccountKey() const {
        return service::utils::SensitiveString(
            detail::privateKeyPem(detail::generateRsaKey().get()));
    }

    template <typename Runtime>
    ruvia::Task<std::string> registerAccount(Runtime& context, std::string_view privateKeyPem,
                                             std::optional<std::string_view> accountEmail,
                                             const std::optional<EabCredentials>& eab) const {
        auto key = detail::loadPrivateKey(privateKeyPem);
        const auto directory = co_await loadDirectory(context);
        AcmeAccountPayloadOutput payload({.resource = context.resource()});
        payload.set<"termsAgreed">(true);
        if (accountEmail && !accountEmail->empty()) {
            const auto contact = "mailto:" + std::string(*accountEmail);
            payload.ensure<"contact">().emplace_back(
                contact, ruvia::ModelOptions{.resource = context.resource()});
        }
        if (eab) {
            payload.set<"externalAccountBinding">(
                buildExternalAccountBinding(context, key.get(), directory.newAccount, *eab));
        }
        const auto response =
            co_await signedPost(context, directory, directory.newAccount, key.get(), std::nullopt,
                                detail::jsonString(std::move(payload), context.resource()));
        const auto location = response.header("location");
        if (!location || location->empty()) {
            throw AcmeError("ACME 账户响应缺少 Location", true);
        }
        (void)targetFromUrl(*location);
        co_return std::string(*location);
    }

    template <typename Runtime>
    ruvia::Task<IssuedCertificate>
    issue(Runtime& context, std::string_view accountPrivateKeyPem, std::string_view accountUrl,
          const std::vector<std::string>& domains, const DnsChallengeConfigView& dns,
          const service::sync_runtime::RunningMarkerLease& certificateTask) const {
        auto accountKey = detail::loadPrivateKey(accountPrivateKeyPem);
        const auto orderSession =
            co_await createOrder(context, accountKey.get(), accountUrl, domains);
        const auto authorizationPlan =
            co_await collectAuthorizationPlan(context, orderSession, accountKey.get(), accountUrl);
        bool challengesPersisted = false;
        std::exception_ptr validationFailure;
        try {
            co_await validateAuthorizations(context, orderSession, authorizationPlan,
                                            accountKey.get(), accountUrl, dns, certificateTask,
                                            challengesPersisted);
        } catch (...) {
            validationFailure = std::current_exception();
        }
        co_await cleanupChallenges(context, dns, certificateTask, challengesPersisted);
        if (validationFailure) {
            std::rethrow_exception(validationFailure);
        }
        co_return co_await finalizeOrder(context, orderSession, accountKey.get(), accountUrl,
                                         domains, certificateTask);
    }

  private:
    template <typename Runtime>
    ruvia::Task<detail::OrderSession> createOrder(Runtime& context, EVP_PKEY* accountKey,
                                                  std::string_view accountUrl,
                                                  const std::vector<std::string>& domains) const {
        auto directory = co_await loadDirectory(context);
        const auto orderPayload = buildOrderPayload(context, domains);
        const auto orderResponse = co_await signedPost(context, directory, directory.newOrder,
                                                       accountKey, accountUrl, orderPayload);
        const auto orderUrlHeader = orderResponse.header("location");
        if (!orderUrlHeader || orderUrlHeader->empty()) {
            throw AcmeError("ACME 订单响应缺少 Location", true);
        }
        std::string orderUrl(*orderUrlHeader);
        (void)targetFromUrl(orderUrl);
        auto order = parseOrder(context, orderResponse.body());
        if (order.authorizations.empty() || order.finalize.empty()) {
            throw AcmeError("ACME 订单缺少验证或 Finalize 地址", true);
        }
        co_return detail::OrderSession{.directory = std::move(directory),
                                       .url = std::move(orderUrl),
                                       .order = std::move(order)};
    }

    template <typename Runtime>
    ruvia::Task<detail::AuthorizationPlan>
    collectAuthorizationPlan(Runtime& context, const detail::OrderSession& orderSession,
                             EVP_PKEY* accountKey, std::string_view accountUrl) const {
        detail::AuthorizationPlan plan;
        const auto thumbprint = detail::jwkThumbprint(accountKey);
        for (const auto& authorizationUrl : orderSession.order.authorizations) {
            auto authorization = co_await loadAuthorization(
                context, orderSession.directory, authorizationUrl, accountKey, accountUrl);
            if (authorization.status == "valid") {
                continue;
            }
            if (authorization.status == "invalid") {
                throw AcmeError(authorization.error.empty()
                                    ? "ACME 授权已失效"
                                    : "ACME 授权失败：" + authorization.error,
                                false);
            }
            if (!authorization.dnsChallenge) {
                throw AcmeError("ACME 授权未提供 DNS-01 Challenge", true);
            }
            const auto keyAuthorization = authorization.dnsChallenge->token + "." + thumbprint;
            const auto digest = detail::sha256(keyAuthorization);
            const auto content = detail::base64Url(digest);
            const auto recordName = challengeRecordName(authorization.identifier);
            plan.challengeSpecs.push_back({.name = recordName, .content = content});
            plan.pending.push_back(detail::PendingAuthorization{
                .authorizationUrl = authorizationUrl,
                .challengeUrl = authorization.dnsChallenge->url,
            });
        }
        co_return plan;
    }

    template <typename Runtime>
    ruvia::Task<void>
    validateAuthorizations(Runtime& context, const detail::OrderSession& orderSession,
                           const detail::AuthorizationPlan& plan, EVP_PKEY* accountKey,
                           std::string_view accountUrl, const DnsChallengeConfigView& dns,
                           const service::sync_runtime::RunningMarkerLease& certificateTask,
                           bool& challengesPersisted) const {
        if (!plan.pending.empty()) {
            const auto dnsTaskId =
                co_await persistDnsChallenges(context, certificateTask, dns, plan.challengeSpecs);
            challengesPersisted = true;
            co_await waitForDnsMarker(context, dnsTaskId, certificateTask);
            if (co_await ruvia::sleepFor(context.worker(), std::chrono::seconds(10),
                                         context.stopToken()) ==
                ruvia::TimerSleepResult::kStopRequested) {
                throw AcmeError("证书签发已停止", false);
            }
            const auto emptyPayload = buildEmptyPayload(context);
            for (const auto& pending : plan.pending) {
                (void)co_await signedPost(context, orderSession.directory, pending.challengeUrl,
                                          accountKey, accountUrl, emptyPayload);
            }
            for (const auto& pending : plan.pending) {
                co_await waitForAuthorization(context, orderSession.directory,
                                              pending.authorizationUrl, accountKey, accountUrl,
                                              certificateTask);
            }
        }
        co_return;
    }

    template <typename Runtime>
    ruvia::Task<void>
    cleanupChallenges(Runtime& context, const DnsChallengeConfigView& dns,
                      const service::sync_runtime::RunningMarkerLease& certificateTask,
                      bool challengesPersisted) const {
        if (!challengesPersisted) {
            co_return;
        }
        std::exception_ptr cleanupFailure;
        try {
            const auto dnsTaskId = co_await retireDnsChallenges(context, dns.dnsZoneId,
                                                                dns.certificateId, certificateTask);
            if (!dnsTaskId.empty()) {
                co_await waitForDnsMarker(context, dnsTaskId, certificateTask);
            }
        } catch (...) {
            cleanupFailure = std::current_exception();
        }
        if (cleanupFailure) {
            std::rethrow_exception(cleanupFailure);
        }
        co_return;
    }

    template <typename Runtime>
    ruvia::Task<IssuedCertificate>
    finalizeOrder(Runtime& context, const detail::OrderSession& orderSession, EVP_PKEY* accountKey,
                  std::string_view accountUrl, const std::vector<std::string>& domains,
                  const service::sync_runtime::RunningMarkerLease& certificateTask) const {
        auto certificateRequest = detail::createCertificateRequest(domains);
        AcmeFinalizePayloadOutput finalizePayload({.resource = context.resource()});
        finalizePayload.set<"csr">(certificateRequest.csr);
        (void)co_await signedPost(
            context, orderSession.directory, orderSession.order.finalize, accountKey, accountUrl,
            detail::jsonString(std::move(finalizePayload), context.resource()));
        const auto order = co_await waitForOrder(context, orderSession.directory, orderSession.url,
                                                 accountKey, accountUrl, certificateTask);
        if (order.certificate.empty()) {
            throw AcmeError("ACME 有效订单缺少证书下载地址", true);
        }
        const auto certificateResponse = co_await signedPost(
            context, orderSession.directory, order.certificate, accountKey, accountUrl, "");
        if (certificateResponse.body().empty()) {
            throw AcmeError("ACME 返回的证书链为空", false);
        }
        co_return detail::inspectCertificate(
            std::move(certificateRequest.privateKeyPem),
            std::string(certificateResponse.body().data(), certificateResponse.body().size()),
            domains);
    }

    template <typename Runtime>
    ruvia::Task<detail::Directory> loadDirectory(Runtime& context) const {
        const std::array headers{ruvia::HttpHeaderView{"accept", "application/json"}};
        const auto response = co_await send(context, {
                                                         .method = "GET",
                                                         .target = settings_.directoryTarget,
                                                         .headers = headers,
                                                     });
        if (!response.status().isSuccessful()) {
            throwHttpProblem(context, response, "ACME Directory 查询失败");
        }
        const std::optional<AcmeDirectoryInput> parsed =
            ruvia::fromJson<AcmeDirectoryInput>(response.body(), {.resource = context.resource()});
        if (!parsed) {
            throw AcmeError("ACME Directory 响应不完整", false);
        }
        const auto& newNonce = parsed->get<"newNonce">();
        const auto& newAccount = parsed->get<"newAccount">();
        const auto& newOrder = parsed->get<"newOrder">();
        if (!newNonce || !newAccount || !newOrder) {
            throw AcmeError("ACME Directory 响应不完整", false);
        }
        detail::Directory directory{
            std::string(newNonce->view()),
            std::string(newAccount->view()),
            std::string(newOrder->view()),
        };
        (void)targetFromUrl(directory.newNonce);
        (void)targetFromUrl(directory.newAccount);
        (void)targetFromUrl(directory.newOrder);
        co_return directory;
    }

    template <typename Runtime>
    ruvia::Task<std::string> fetchNonce(Runtime& context,
                                        const detail::Directory& directory) const {
        const auto target = targetFromUrl(directory.newNonce);
        const auto response = co_await send(context, {.method = "HEAD", .target = target});
        if (!response.status().isSuccessful()) {
            throwHttpProblem(context, response, "ACME Nonce 获取失败");
        }
        const auto nonce = response.header("replay-nonce");
        if (!nonce || nonce->empty()) {
            throw AcmeError("ACME Nonce 响应缺少 Replay-Nonce", false);
        }
        co_return std::string(*nonce);
    }

    template <typename Runtime>
    ruvia::Task<service::outbound_http::BufferedResponse>
    signedPost(Runtime& context, const detail::Directory& directory, std::string_view url,
               EVP_PKEY* key, std::optional<std::string_view> accountUrl,
               std::string_view payload) const {
        for (int attempt = 0; attempt < 2; ++attempt) {
            const auto nonce = co_await fetchNonce(context, directory);
            const auto body = buildJws(context, url, nonce, key, accountUrl, payload);
            const auto target = targetFromUrl(url);
            const std::array headers{
                ruvia::HttpHeaderView{"content-type", "application/jose+json"},
                ruvia::HttpHeaderView{"accept",
                                      "application/json, application/pem-certificate-chain"},
            };
            auto response = co_await send(
                context, {
                             .method = "POST",
                             .target = target,
                             .headers = headers,
                             .content = ruvia::HttpClientRequestContentView::bytes(body),
                         });
            if (response.status().isSuccessful()) {
                co_return response;
            }
            if (attempt == 0 && isBadNonce(context, response)) {
                continue;
            }
            throwHttpProblem(context, response, "ACME 请求失败");
        }
        throw AcmeError("ACME Nonce 重试失败", false);
    }

    template <typename Runtime>
    ruvia::Task<service::outbound_http::BufferedResponse>
    send(Runtime& context, const ruvia::HttpClientRequestView& request) const {
        try {
            auto&& client = context.httpClient(
                service::config::outboundOriginAlias(service::config::acmeOrigin(settings_.host)));
            co_return co_await service::outbound_http::sendBuffered(
                client, request,
                {.timeout = std::chrono::seconds(30), .stopToken = context.stopToken()});
        } catch (const ruvia::HttpClientError& error) {
            using Code = ruvia::HttpClientError::Code;
            switch (error.code()) {
            case Code::kTimeout:
                throw AcmeError("ACME 请求超时（30 秒）", false);
            case Code::kResolveFailed:
                throw AcmeError("ACME 服务域名解析失败", false);
            case Code::kConnectFailed:
                throw AcmeError("ACME 服务连接失败", false);
            case Code::kTlsFailed:
                throw AcmeError("ACME TLS 连接失败", false);
            default:
                throw AcmeError("ACME HTTP 客户端失败", false);
            }
        }
    }

    template <typename Runtime>
    std::string buildJws(Runtime& context, std::string_view url, std::string_view nonce,
                         EVP_PKEY* key, std::optional<std::string_view> accountUrl,
                         std::string_view payload) const {
        std::string protectedJson;
        if (accountUrl) {
            AcmeProtectedKidOutput protectedValue({.resource = context.resource()});
            protectedValue.set<"alg">("RS256");
            protectedValue.set<"kid">(*accountUrl);
            protectedValue.set<"nonce">(nonce);
            protectedValue.set<"url">(url);
            protectedJson = detail::jsonString(std::move(protectedValue), context.resource());
        } else {
            const auto keyData = detail::jwk(key);
            AcmeJwkOutput jwkValue({.resource = context.resource()});
            jwkValue.set<"e">(keyData.exponent);
            jwkValue.set<"kty">("RSA");
            jwkValue.set<"n">(keyData.modulus);
            AcmeProtectedJwkOutput protectedValue({.resource = context.resource()});
            protectedValue.set<"alg">("RS256");
            protectedValue.set<"jwk">(std::move(jwkValue));
            protectedValue.set<"nonce">(nonce);
            protectedValue.set<"url">(url);
            protectedJson = detail::jsonString(std::move(protectedValue), context.resource());
        }
        const auto protectedEncoded = detail::base64Url(protectedJson);
        const auto payloadEncoded = detail::base64Url(payload);
        const auto signature = detail::sign(key, protectedEncoded + "." + payloadEncoded);
        AcmeJwsOutput jws({.resource = context.resource()});
        jws.set<"protectedValue">(protectedEncoded);
        jws.set<"payload">(payloadEncoded);
        jws.set<"signature">(signature);
        return detail::jsonString(std::move(jws), context.resource());
    }

    template <typename Runtime>
    AcmeJwsOutput buildExternalAccountBinding(Runtime& context, EVP_PKEY* key,
                                              std::string_view newAccountUrl,
                                              const EabCredentials& credentials) const {
        const auto keyData = detail::jwk(key);
        AcmeJwkOutput accountJwk({.resource = context.resource()});
        accountJwk.set<"e">(keyData.exponent);
        accountJwk.set<"kty">("RSA");
        accountJwk.set<"n">(keyData.modulus);
        const auto payload = detail::jsonString(std::move(accountJwk), context.resource());

        AcmeEabProtectedOutput protectedValue({.resource = context.resource()});
        protectedValue.set<"alg">("HS256");
        protectedValue.set<"kid">(credentials.keyId);
        protectedValue.set<"url">(newAccountUrl);
        const auto protectedJson =
            detail::jsonString(std::move(protectedValue), context.resource());
        const auto protectedEncoded = detail::base64Url(protectedJson);
        const auto payloadEncoded = detail::base64Url(payload);

        AcmeJwsOutput result({.resource = context.resource()});
        result.set<"protectedValue">(protectedEncoded);
        result.set<"payload">(payloadEncoded);
        result.set<"signature">(detail::hmacSha256(credentials.hmacKey.view(),
                                                   protectedEncoded + "." + payloadEncoded));
        return result;
    }

    template <typename Runtime>
    std::string buildOrderPayload(Runtime& context, const std::vector<std::string>& domains) const {
        AcmeOrderPayloadOutput payload({.resource = context.resource()});
        auto& identifiers = payload.ensure<"identifiers">();
        for (const auto& domain : domains) {
            auto& identifier =
                identifiers.emplace_back(ruvia::ModelOptions{.resource = context.resource()});
            identifier.set<"type">("dns");
            identifier.set<"value">(domain);
        }
        return detail::jsonString(std::move(payload), context.resource());
    }

    template <typename Runtime> std::string buildEmptyPayload(Runtime& context) const {
        AcmeEmptyPayloadOutput payload({.resource = context.resource()});
        return detail::jsonString(std::move(payload), context.resource());
    }

    template <typename Runtime>
    detail::Order parseOrder(Runtime& context, std::string_view body) const {
        const std::optional<AcmeOrderInput> parsed =
            ruvia::fromJson<AcmeOrderInput>(body, {.resource = context.resource()});
        if (!parsed) {
            throw AcmeError("ACME 订单响应格式无效", false);
        }
        detail::Order result;
        result.status = detail::optionalString(parsed->get<"status">());
        result.finalize = detail::optionalString(parsed->get<"finalize">());
        result.certificate = detail::optionalString(parsed->get<"certificate">());
        result.error = detail::problemDetail(parsed->get<"error">());
        if (const auto& values = parsed->get<"authorizations">(); values) {
            result.authorizations.reserve(values->size());
            for (const auto& value : *values) {
                result.authorizations.emplace_back(value.view());
            }
        }
        return result;
    }

    template <typename Runtime>
    detail::Authorization parseAuthorization(Runtime& context, std::string_view body) const {
        const std::optional<AcmeAuthorizationInput> parsed =
            ruvia::fromJson<AcmeAuthorizationInput>(body, {.resource = context.resource()});
        if (!parsed) {
            throw AcmeError("ACME 授权响应格式无效", false);
        }
        detail::Authorization result;
        result.status = detail::optionalString(parsed->get<"status">());
        if (const auto& identifier = parsed->get<"identifier">(); identifier) {
            result.identifier = detail::optionalString(identifier->get<"value">());
        }
        if (const auto& challenges = parsed->get<"challenges">(); challenges) {
            for (const auto& challenge : *challenges) {
                const auto type = detail::optionalString(challenge.get<"type">());
                if (type != "dns-01") {
                    continue;
                }
                const auto url = detail::optionalString(challenge.get<"url">());
                const auto token = detail::optionalString(challenge.get<"token">());
                if (!url.empty() && !token.empty()) {
                    result.dnsChallenge = detail::Challenge{url, token};
                }
                result.error = detail::problemDetail(challenge.get<"error">());
                break;
            }
        }
        return result;
    }

    template <typename Runtime>
    ruvia::Task<detail::Authorization>
    loadAuthorization(Runtime& context, const detail::Directory& directory, std::string_view url,
                      EVP_PKEY* key, std::string_view accountUrl) const {
        const auto response =
            co_await signedPost(context, directory, url, key, accountUrl, std::string_view{});
        co_return parseAuthorization(context, response.body());
    }

    template <typename Runtime>
    ruvia::Task<void>
    waitForAuthorization(Runtime& context, const detail::Directory& directory,
                         std::string_view authorizationUrl, EVP_PKEY* key,
                         std::string_view accountUrl,
                         const service::sync_runtime::RunningMarkerLease& certificateTask) const {
        for (int attempt = 0; attempt < 60; ++attempt) {
            if (!co_await service::sync_runtime::renewRunningLease(context.db(), certificateTask)) {
                throw std::runtime_error("证书任务 lease 已失效");
            }
            const auto authorization =
                co_await loadAuthorization(context, directory, authorizationUrl, key, accountUrl);
            if (authorization.status == "valid") {
                co_return;
            }
            if (authorization.status == "invalid") {
                throw AcmeError(authorization.error.empty()
                                    ? "ACME DNS-01 验证失败"
                                    : "ACME DNS-01 验证失败：" + authorization.error,
                                false);
            }
            if (co_await ruvia::sleepFor(context.worker(), std::chrono::seconds(2),
                                         context.stopToken()) ==
                ruvia::TimerSleepResult::kStopRequested) {
                throw AcmeError("证书签发已停止", false);
            }
        }
        throw AcmeError("ACME DNS-01 验证等待超时", false);
    }

    template <typename Runtime>
    ruvia::Task<detail::Order>
    waitForOrder(Runtime& context, const detail::Directory& directory, std::string_view orderUrl,
                 EVP_PKEY* key, std::string_view accountUrl,
                 const service::sync_runtime::RunningMarkerLease& certificateTask) const {
        for (int attempt = 0; attempt < 60; ++attempt) {
            if (!co_await service::sync_runtime::renewRunningLease(context.db(), certificateTask)) {
                throw std::runtime_error("证书任务 lease 已失效");
            }
            const auto response = co_await signedPost(context, directory, orderUrl, key, accountUrl,
                                                      std::string_view{});
            auto order = parseOrder(context, response.body());
            if (order.status == "valid") {
                co_return order;
            }
            if (order.status == "invalid") {
                throw AcmeError(order.error.empty() ? "ACME 证书订单失败"
                                                    : "ACME 证书订单失败：" + order.error,
                                true);
            }
            if (co_await ruvia::sleepFor(context.worker(), std::chrono::seconds(2),
                                         context.stopToken()) ==
                ruvia::TimerSleepResult::kStopRequested) {
                throw AcmeError("证书签发已停止", false);
            }
        }
        throw AcmeError("ACME 证书签发等待超时", false);
    }

    [[nodiscard]] std::string targetFromUrl(std::string_view url) const {
        const auto prefix = "https://" + settings_.host;
        if (!url.starts_with(prefix)) {
            throw AcmeError("ACME 响应返回了未配置的服务地址", true);
        }
        const auto target = url.substr(prefix.size());
        if (target.empty() || target.front() != '/') {
            throw AcmeError("ACME 服务地址格式无效", true);
        }
        return std::string(target);
    }

    static std::string challengeRecordName(std::string_view identifier) {
        if (identifier.starts_with("*.")) {
            identifier.remove_prefix(2);
        }
        if (identifier.empty()) {
            throw AcmeError("ACME 授权缺少域名标识", true);
        }
        return "_acme-challenge." + std::string(identifier);
    }

    static bool permanentProblem(std::string_view type, ruvia::HttpStatusCode status) {
        if (status.value() == 429 || status.isServerError()) {
            return false;
        }
        return type.ends_with(":malformed") || type.ends_with(":unauthorized") ||
               type.ends_with(":rejectedIdentifier") || type.ends_with(":accountDoesNotExist");
    }

    template <typename Runtime>
    static bool isBadNonce(Runtime& context,
                           const service::outbound_http::BufferedResponse& response) {
        const std::optional<AcmeProblemInput> parsed =
            ruvia::fromJson<AcmeProblemInput>(response.body(), {.resource = context.resource()});
        if (!parsed) {
            return false;
        }
        const auto& type = parsed->get<"type">();
        return type && type->view().ends_with(":badNonce");
    }

    template <typename Runtime>
    [[noreturn]] static void
    throwHttpProblem(Runtime& context, const service::outbound_http::BufferedResponse& response,
                     std::string_view fallback) {
        const std::optional<AcmeProblemInput> parsed =
            ruvia::fromJson<AcmeProblemInput>(response.body(), {.resource = context.resource()});
        std::string type;
        std::string message(fallback);
        if (parsed) {
            type = detail::optionalString(parsed->get<"type">());
            const auto detailValue = detail::optionalString(parsed->get<"detail">());
            if (!detailValue.empty()) {
                message.append("：").append(detailValue);
            }
        }
        throw AcmeError(std::move(message), permanentProblem(type, response.status()));
    }

    AcmeSettings settings_;
};

template <typename Runtime>
ruvia::Task<AcmeAccount>
ensureAcmeAccount(Runtime& context, std::string_view tenantId, std::string_view providerId,
                  std::int64_t expectedRevision, std::optional<std::string_view> accountEmail,
                  const AcmeClient& client, const std::optional<EabCredentials>& eab) {
    const auto rows = co_await context.db().query(
        "SELECT revision, runtime::text FROM sys_provider WHERE tenant_id = $1 AND id = $2 AND "
        "kind = 'certificate' AND deleted_at IS NULL LIMIT 1",
        tenantId, providerId);
    if (rows.empty()) {
        throw AcmeError("证书供应商不存在", true);
    }
    if (rows.front()[0].template as<std::int64_t>().value_or(0) != expectedRevision) {
        throw AcmeError("证书供应商配置已变更", false);
    }
    const auto runtime = parseCertificateProviderRuntime(rows.front()[1].value().value_or("{}"),
                                                         {.resource = context.resource()});
    if (!runtime) {
        throw AcmeError("证书供应商运行状态损坏", true);
    }
    AcmeAccount account;
    std::string privateKeyEnvelope;
    bool hasStoredPrivateKey = false;
    if (runtime->acmeAccount) {
        const auto& acmeAccount = *runtime->acmeAccount;
        if (acmeAccount.privateKeyEnvelope) {
            hasStoredPrivateKey = true;
            privateKeyEnvelope = *acmeAccount.privateKeyEnvelope;
            account.privateKeyPem =
                service::utils::SensitiveString(service::utils::openSecret(privateKeyEnvelope));
            if (acmeAccount.accountUrl) {
                account.accountUrl = *acmeAccount.accountUrl;
            }
        }
    }
    if (!hasStoredPrivateKey) {
        auto generatedPrivateKey = client.generateAccountKey();
        privateKeyEnvelope = service::utils::sealSecret(generatedPrivateKey.view());
        account.privateKeyPem = std::move(generatedPrivateKey);
    }
    if (account.accountUrl.empty()) {
        account.accountUrl = co_await client.registerAccount(context, account.privateKeyPem.view(),
                                                             accountEmail, eab);
    }

    auto transaction = co_await context.db().beginTransaction();
    const auto currentRows = co_await transaction.query(
        "SELECT revision, runtime::text FROM sys_provider WHERE tenant_id = $1 AND id = $2 "
        "AND kind = 'certificate' AND deleted_at IS NULL LIMIT 1 FOR UPDATE",
        tenantId, providerId);
    if (currentRows.empty() ||
        currentRows.front()[0].template as<std::int64_t>().value_or(0) != expectedRevision) {
        throw AcmeError("证书供应商配置已变更", false);
    }
    const auto current = parseCertificateProviderRuntime(
        currentRows.front()[1].value().value_or("{}"), {.resource = context.resource()});
    if (!current) {
        throw AcmeError("证书供应商运行状态损坏", true);
    }
    if (current->acmeAccount && current->acmeAccount->privateKeyEnvelope &&
        current->acmeAccount->accountUrl) {
        AcmeAccount persisted{
            .privateKeyPem = service::utils::SensitiveString(
                service::utils::openSecret(*current->acmeAccount->privateKeyEnvelope)),
            .accountUrl = *current->acmeAccount->accountUrl,
        };
        co_await transaction.commit();
        co_return persisted;
    }
    CertificateProviderRuntimeOutput output = toOutput(*current, {.resource = context.resource()});
    auto& acmeAccount = output.ensure<"acmeAccount">();
    acmeAccount.set<"privateKeyEnvelope">(privateKeyEnvelope);
    acmeAccount.set<"accountUrl">(account.accountUrl);
    const auto runtimeJson = ruvia::toJson(output, {.resource = context.resource()});
    const auto updated = co_await transaction.execute(
        "UPDATE sys_provider SET runtime = $1::jsonb, updated_at = NOW() WHERE tenant_id = "
        "$2 AND id = $3 AND kind = 'certificate' AND revision = $4 AND deleted_at IS NULL",
        std::string_view(runtimeJson), tenantId, providerId, expectedRevision);
    if (updated.affectedRows() == 0) {
        throw AcmeError("证书供应商配置已变更", false);
    }
    co_await transaction.commit();
    co_return account;
}

} // namespace service::certificate_issuance
