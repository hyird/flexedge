#pragma once

#include <array>
#include <cstddef>
#include <ctime>
#include <iomanip>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include "service/features/certificate/acme_jws.h"
#include "service/features/certificate/acme_types.h"
#include "service/utils/sensitive_string.h"

namespace service::certificate_issuance::detail {

using CertificateX509Ptr = std::unique_ptr<X509, decltype(&X509_free)>;
using CertificateBioPtr = std::unique_ptr<BIO, decltype(&BIO_free)>;
using CertificateBnPtr = std::unique_ptr<BIGNUM, decltype(&BN_free)>;

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
    CertificateBnPtr number(ASN1_INTEGER_to_BN(X509_get_serialNumber(certificate), nullptr),
                            BN_free);
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
    CertificateBioPtr bio(
        BIO_new_mem_buf(certificateChain.data(), static_cast<int>(certificateChain.size())),
        BIO_free);
    if (!bio) {
        throw AcmeError("证书链读取失败", true);
    }
    CertificateX509Ptr certificate(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr),
                                   X509_free);
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

} // namespace service::certificate_issuance::detail
