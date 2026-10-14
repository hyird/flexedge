#pragma once

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "service/utils/sensitive_string.h"

namespace service::certificate_issuance {

struct acme_settings final {
    std::string host_;
    std::string directory_target_;
    std::string directory_url_;
};

inline std::optional<acme_settings> settings_for_provider(std::string_view provider) {
    if (provider == "letsencrypt") {
        return acme_settings{
            .host_ = "acme-v02.api.letsencrypt.org",
            .directory_target_ = "/directory",
            .directory_url_ = "https://acme-v02.api.letsencrypt.org/directory",
        };
    }
    if (provider == "zerossl") {
        return acme_settings{
            .host_ = "acme.zerossl.com",
            .directory_target_ = "/v2/DV90",
            .directory_url_ = "https://acme.zerossl.com/v2/DV90",
        };
    }
    return std::nullopt;
}

class acme_error final : public std::runtime_error {
public:
    acme_error(std::string_view message, bool permanent)
        : std::runtime_error(std::string(message)),
          permanent_(permanent) {}

    [[nodiscard]] bool permanent() const noexcept {
        return permanent_;
    }

private:
    bool permanent_;
};

struct eab_credentials final {
    std::string key_id_;
    service::utils::sensitive_string hmac_key_;
};

struct acme_account final {
    service::utils::sensitive_string private_key_pem_;
    std::string account_url_;
};

struct issued_certificate final {
    service::utils::sensitive_string private_key_pem_;
    std::string certificate_chain_pem_;
    std::string not_before_;
    std::string expires_at_;
    std::string serial_number_;
    std::string fingerprint_sha256_;
};

}  // namespace service::certificate_issuance
