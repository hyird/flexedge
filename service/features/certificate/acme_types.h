#pragma once

#include <exception>
#include <optional>
#include <stdexcept>
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

enum class failure_disposition {
    resume_order,
    restart_order,
    terminal,
};

struct issuance_failure final {
    std::string message_;
    failure_disposition disposition_;
};

// One decision controls task completion, durable progress and DNS cleanup.
[[nodiscard]] inline issuance_failure classify_issuance_failure(const std::exception_ptr& exception) {
    try {
        std::rethrow_exception(exception);
    } catch (const acme_pending& error) {
        return {.message_ = error.what(), .disposition_ = failure_disposition::resume_order};
    } catch (const service::outbound_http::transport_error& error) {
        return {.message_ = error.what(), .disposition_ = failure_disposition::resume_order};
    } catch (const service::outbound_http::attempt_error& error) {
        return {.message_ = error.what(),
            .disposition_ = error.permanent() ? failure_disposition::terminal : failure_disposition::restart_order};
    } catch (const std::invalid_argument& error) {
        return {.message_ = error.what(), .disposition_ = failure_disposition::terminal};
    } catch (const std::exception& error) {
        return {.message_ = error.what(), .disposition_ = failure_disposition::restart_order};
    } catch (...) {
        return {.message_ = "证书任务发生未知错误", .disposition_ = failure_disposition::restart_order};
    }
}

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
