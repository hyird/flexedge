#pragma once

#include <ruvia/web/model_json.h>

#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "service/features/certificate/provider_config_model.h"
#include "service/features/certificate/provider_config_transport.h"

namespace service::certificate_issuance {

[[nodiscard]] inline certificate_provider_eab_runtime_data
normalize(const certificate_provider_eab_runtime_input& input) {
    certificate_provider_eab_runtime_data result;
    if (const auto& value = input.get<"kid">()) {
        result.kid_ = std::string(value->view());
    }
    if (const auto& value = input.get<"hmac_key_envelope">()) {
        result.hmac_key_envelope_ = std::string(value->view());
    }
    return result;
}

[[nodiscard]] inline certificate_provider_account_runtime_data
normalize(const certificate_provider_account_runtime_input& input) {
    certificate_provider_account_runtime_data result;
    if (const auto& value = input.get<"private_key_envelope">()) {
        result.private_key_envelope_ = std::string(value->view());
    }
    if (const auto& value = input.get<"account_url">()) {
        result.account_url_ = std::string(value->view());
    }
    return result;
}

[[nodiscard]] inline certificate_provider_runtime_data
normalize(const certificate_provider_runtime_input& input) {
    certificate_provider_runtime_data result;
    if (const auto& eab = input.get<"eab">()) {
        result.eab_ = normalize(*eab);
    }
    if (const auto& account = input.get<"acme_account">()) {
        result.acme_account_ = normalize(*account);
    }
    return result;
}

[[nodiscard]] inline std::optional<certificate_provider_runtime_data>
parse_certificate_provider_runtime(std::string_view json, ruvia::model_parse_options options = {}) {
    const std::optional<certificate_provider_runtime_input> input =
        ruvia::from_json<certificate_provider_runtime_input>(json, options);
    return input ? std::optional<certificate_provider_runtime_data>{normalize(*input)} : std::nullopt;
}

[[nodiscard]] inline certificate_provider_runtime_output
to_output(const certificate_provider_runtime_data& input, ruvia::model_options options = {}) {
    certificate_provider_runtime_output output(options);
    if (input.eab_ && input.eab_->kid_ && input.eab_->hmac_key_envelope_) {
        auto& eab = output.ensure<"eab">();
        eab.set<"kid">(*input.eab_->kid_);
        eab.set<"hmac_key_envelope">(*input.eab_->hmac_key_envelope_);
    }
    if (input.acme_account_ && input.acme_account_->private_key_envelope_ &&
        input.acme_account_->account_url_) {
        auto& acme_account = output.ensure<"acme_account">();
        acme_account.set<"private_key_envelope">(*input.acme_account_->private_key_envelope_);
        acme_account.set<"account_url">(*input.acme_account_->account_url_);
    }
    return output;
}

[[nodiscard]] inline std::optional<certificate_provider_config_data>
normalize(const certificate_provider_config_input& input) {
    const auto& credential_mode = input.get<"credential_mode">();
    if (!credential_mode) {
        return std::nullopt;
    }

    certificate_provider_config_data result{
        .credential_mode_ = std::string(credential_mode->view()),
    };
    if (const auto& account_email = input.get<"account_email">()) {
        result.account_email_ = std::string(account_email->view());
    }
    if (const auto& access_key = input.get<"access_key">()) {
        const auto& envelope = access_key->get<"envelope">();
        const auto& hint = access_key->get<"hint">();
        if (!envelope || !hint) {
            return std::nullopt;
        }
        result.envelope_ = std::string(envelope->view());
        result.hint_ = std::string(hint->view());
    }
    return result;
}

inline certificate_provider_config_data
parse_certificate_provider_config(std::string_view json, std::pmr::memory_resource* resource) {
    const std::optional<certificate_provider_config_input> config =
        ruvia::from_json<certificate_provider_config_input>(json, {.resource_ = resource});
    if (!config) {
        throw std::runtime_error("stored certificate provider config is incomplete");
    }
    const auto normalized = normalize(*config);
    if (!normalized) {
        throw std::runtime_error("stored certificate provider config is incomplete");
    }
    return *normalized;
}

[[nodiscard]] inline certificate_provider_config_output
to_output(const certificate_provider_config_data& input, ruvia::model_options options = {}) {
    certificate_provider_config_output output(options);
    output.set<"credential_mode">(input.credential_mode_);
    if (input.account_email_) {
        output.set<"account_email">(*input.account_email_);
    }
    if (input.envelope_ && input.hint_) {
        auto& access_key = output.ensure<"access_key">();
        access_key.set<"envelope">(*input.envelope_);
        access_key.set<"hint">(*input.hint_);
    }
    return output;
}

inline std::string serialize_certificate_provider_config(
    std::string_view credential_mode, const std::optional<std::string>& account_email,
    const std::optional<std::string>& envelope, const std::optional<std::string>& hint,
    std::pmr::memory_resource* resource) {
    const auto output =
        to_output(certificate_provider_config_data{.credential_mode_ = std::string(credential_mode),
                      .account_email_ = account_email,
                      .envelope_ = envelope,
                      .hint_ = hint},
            {.resource_ = resource});
    const auto json = ruvia::to_json(output, {.resource_ = resource});
    return std::string(json.data(), json.size());
}
}  // namespace service::certificate_issuance
