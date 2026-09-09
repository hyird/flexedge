#pragma once

#include <optional>
#include <string>

namespace service::certificate_issuance {

struct CertificateProviderEabRuntimeData final {
    std::optional<std::string> kid;
    std::optional<std::string> hmacKeyEnvelope;
};

struct CertificateProviderAccountRuntimeData final {
    std::optional<std::string> privateKeyEnvelope;
    std::optional<std::string> accountUrl;
};

struct CertificateProviderRuntimeData final {
    std::optional<CertificateProviderEabRuntimeData> eab;
    std::optional<CertificateProviderAccountRuntimeData> acmeAccount;
};

struct CertificateProviderConfigData final {
    std::string credentialMode;
    std::optional<std::string> accountEmail{};
    std::optional<std::string> envelope{};
    std::optional<std::string> hint{};
};
} // namespace service::certificate_issuance
