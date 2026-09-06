#pragma once

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "service/utils/sensitive_string.h"

namespace service::certificate_issuance {

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

} // namespace service::certificate_issuance
