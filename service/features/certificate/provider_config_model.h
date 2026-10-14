#pragma once

#include <optional>
#include <string>

namespace service::certificate_issuance {

struct certificate_provider_eab_runtime_data final {
    std::optional<std::string> kid_;
    std::optional<std::string> hmac_key_envelope_;
};

struct certificate_provider_account_runtime_data final {
    std::optional<std::string> private_key_envelope_;
    std::optional<std::string> account_url_;
};

struct certificate_provider_runtime_data final {
    std::optional<certificate_provider_eab_runtime_data> eab_;
    std::optional<certificate_provider_account_runtime_data> acme_account_;
};

struct certificate_provider_config_data final {
    std::string credential_mode_;
    std::optional<std::string> account_email_{};
    std::optional<std::string> envelope_{};
    std::optional<std::string> hint_{};
};
}  // namespace service::certificate_issuance
