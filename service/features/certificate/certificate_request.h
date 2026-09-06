#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <openssl/asn1.h>
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include "service/features/certificate/acme_jws.h"
#include "service/features/certificate/acme_types.h"
#include "service/utils/sensitive_string.h"

namespace service::certificate_issuance::detail {

using CertificateRequestPtr = std::unique_ptr<X509_REQ, decltype(&X509_REQ_free)>;

struct CertificateRequest final {
    service::utils::SensitiveString privateKeyPem;
    std::string csr;
};

inline CertificateRequest createCertificateRequest(const std::vector<std::string>& domains) {
    if (domains.empty()) {
        throw AcmeError("证书标识不能为空", true);
    }
    auto key = generateRsaKey();
    CertificateRequestPtr request(X509_REQ_new(), X509_REQ_free);
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

} // namespace service::certificate_issuance::detail
