#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "service/features/outbound_http/error.h"
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

// A failure the ACME server reported or the issuance flow detected.
class acme_error : public service::outbound_http::attempt_error {
public:
    acme_error(std::string_view message, bool permanent)
        : attempt_error(message, permanent) {}
};

// The ACME server has not finished an authorization or order within the
// attempt's polling window (or the worker stops): the order stays usable and
// the next attempt resumes it.
class acme_pending final : public acme_error {
public:
    explicit acme_pending(std::string_view message)
        : acme_error(message, false) {}
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
    // Issuer distinguished name in RFC 2253 form, e.g. "CN=R11,O=Let's Encrypt,C=US".
    std::string issuer_;
};

}  // namespace service::certificate_issuance
