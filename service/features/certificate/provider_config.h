#pragma once

#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <ruvia/web/ModelJson.h>

#include "service/features/certificate/model.h"
#include "service/utils/secret.h"
#include "service/utils/sensitive_string.h"

namespace service::certificate_issuance {

struct EabCredentials final {
    std::string keyId;
    service::utils::SensitiveString hmacKey;
};

RUVIA_REQUEST_MODEL(CertificateProviderEabRuntimeInput, RUVIA_OPTIONAL_FIELD(kid, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("hmac_key_envelope", hmacKeyEnvelope, ruvia::String));
RUVIA_REQUEST_MODEL(CertificateProviderAccountRuntimeInput,
                    RUVIA_OPTIONAL_FIELD_NAME("private_key_envelope", privateKeyEnvelope,
                                              ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("account_url", accountUrl, ruvia::String));
RUVIA_REQUEST_MODEL(CertificateProviderRuntimeInput,
                    RUVIA_OPTIONAL_FIELD(eab, CertificateProviderEabRuntimeInput),
                    RUVIA_OPTIONAL_FIELD_NAME("acme_account", acmeAccount,
                                              CertificateProviderAccountRuntimeInput));
RUVIA_RESPONSE_MODEL(CertificateProviderEabRuntimeOutput, RUVIA_REQUIRED_FIELD(kid, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("hmac_key_envelope", hmacKeyEnvelope,
                                               ruvia::String));
RUVIA_RESPONSE_MODEL(CertificateProviderAccountRuntimeOutput,
                     RUVIA_REQUIRED_FIELD_NAME("private_key_envelope", privateKeyEnvelope,
                                               ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("account_url", accountUrl, ruvia::String));
RUVIA_RESPONSE_MODEL(CertificateProviderRuntimeOutput,
                     RUVIA_OPTIONAL_FIELD(eab, CertificateProviderEabRuntimeOutput,
                                          RUVIA_OMIT_EMPTY),
                     RUVIA_OPTIONAL_FIELD_NAME("acme_account", acmeAccount,
                                               CertificateProviderAccountRuntimeOutput,
                                               RUVIA_OMIT_EMPTY));

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

[[nodiscard]] inline CertificateProviderEabRuntimeData
normalize(const CertificateProviderEabRuntimeInput& input) {
    CertificateProviderEabRuntimeData result;
    if (const auto& value = input.get<"kid">()) {
        result.kid = std::string(value->view());
    }
    if (const auto& value = input.get<"hmacKeyEnvelope">()) {
        result.hmacKeyEnvelope = std::string(value->view());
    }
    return result;
}

[[nodiscard]] inline CertificateProviderAccountRuntimeData
normalize(const CertificateProviderAccountRuntimeInput& input) {
    CertificateProviderAccountRuntimeData result;
    if (const auto& value = input.get<"privateKeyEnvelope">()) {
        result.privateKeyEnvelope = std::string(value->view());
    }
    if (const auto& value = input.get<"accountUrl">()) {
        result.accountUrl = std::string(value->view());
    }
    return result;
}

[[nodiscard]] inline CertificateProviderRuntimeData
normalize(const CertificateProviderRuntimeInput& input) {
    CertificateProviderRuntimeData result;
    if (const auto& eab = input.get<"eab">()) {
        result.eab = normalize(*eab);
    }
    if (const auto& account = input.get<"acmeAccount">()) {
        result.acmeAccount = normalize(*account);
    }
    return result;
}

[[nodiscard]] inline std::optional<CertificateProviderRuntimeData>
parseCertificateProviderRuntime(std::string_view json, ruvia::ModelParseOptions options = {}) {
    const std::optional<CertificateProviderRuntimeInput> input =
        ruvia::fromJson<CertificateProviderRuntimeInput>(json, options);
    return input ? std::optional<CertificateProviderRuntimeData>{normalize(*input)} : std::nullopt;
}

[[nodiscard]] inline CertificateProviderRuntimeOutput
toOutput(const CertificateProviderRuntimeData& input, ruvia::ModelOptions options = {}) {
    CertificateProviderRuntimeOutput output(options);
    if (input.eab && input.eab->kid && input.eab->hmacKeyEnvelope) {
        auto& eab = output.ensure<"eab">();
        eab.set<"kid">(*input.eab->kid);
        eab.set<"hmacKeyEnvelope">(*input.eab->hmacKeyEnvelope);
    }
    if (input.acmeAccount && input.acmeAccount->privateKeyEnvelope &&
        input.acmeAccount->accountUrl) {
        auto& acmeAccount = output.ensure<"acmeAccount">();
        acmeAccount.set<"privateKeyEnvelope">(*input.acmeAccount->privateKeyEnvelope);
        acmeAccount.set<"accountUrl">(*input.acmeAccount->accountUrl);
    }
    return output;
}

[[nodiscard]] inline std::optional<CertificateProviderConfigData>
normalize(const CertificateProviderConfigInput& input) {
    const auto& credentialMode = input.get<"credentialMode">();
    if (!credentialMode) {
        return std::nullopt;
    }

    CertificateProviderConfigData result{
        .credentialMode = std::string(credentialMode->view()),
    };
    if (const auto& accountEmail = input.get<"accountEmail">()) {
        result.accountEmail = std::string(accountEmail->view());
    }
    if (const auto& accessKey = input.get<"accessKey">()) {
        const auto& envelope = accessKey->get<"envelope">();
        const auto& hint = accessKey->get<"hint">();
        if (!envelope || !hint) {
            return std::nullopt;
        }
        result.envelope = std::string(envelope->view());
        result.hint = std::string(hint->view());
    }
    return result;
}

inline CertificateProviderConfigData
parseCertificateProviderConfig(std::string_view json, std::pmr::memory_resource* resource) {
    const std::optional<CertificateProviderConfigInput> config =
        ruvia::fromJson<CertificateProviderConfigInput>(json, {.resource = resource});
    if (!config) {
        throw std::runtime_error("stored certificate provider config is incomplete");
    }
    const auto normalized = normalize(*config);
    if (!normalized) {
        throw std::runtime_error("stored certificate provider config is incomplete");
    }
    return *normalized;
}

[[nodiscard]] inline CertificateProviderConfigOutput
toOutput(const CertificateProviderConfigData& input, ruvia::ModelOptions options = {}) {
    CertificateProviderConfigOutput output(options);
    output.set<"credentialMode">(input.credentialMode);
    if (input.accountEmail) {
        output.set<"accountEmail">(*input.accountEmail);
    }
    if (input.envelope && input.hint) {
        auto& accessKey = output.ensure<"accessKey">();
        accessKey.set<"envelope">(*input.envelope);
        accessKey.set<"hint">(*input.hint);
    }
    return output;
}

inline std::string serializeCertificateProviderConfig(
    std::string_view credentialMode, const std::optional<std::string>& accountEmail,
    const std::optional<std::string>& envelope, const std::optional<std::string>& hint,
    std::pmr::memory_resource* resource) {
    const auto output =
        toOutput(CertificateProviderConfigData{.credentialMode = std::string(credentialMode),
                                               .accountEmail = accountEmail,
                                               .envelope = envelope,
                                               .hint = hint},
                 {.resource = resource});
    const auto json = ruvia::toJson(output, {.resource = resource});
    return std::string(json.data(), json.size());
}

inline std::string
serializeCertificateProviderRuntime(const std::optional<CertificateProviderRuntimeData>& runtime,
                                    const std::optional<EabCredentials>& eab,
                                    std::pmr::memory_resource* resource) {
    CertificateProviderRuntimeOutput output =
        runtime ? toOutput(*runtime, {.resource = resource})
                : CertificateProviderRuntimeOutput({.resource = resource});
    if (eab) {
        auto& eabOutput = output.ensure<"eab">();
        eabOutput.set<"kid">(eab->keyId);
        eabOutput.set<"hmacKeyEnvelope">(service::utils::sealSecret(eab->hmacKey.view()));
    }
    const auto json = ruvia::toJson(output, {.resource = resource});
    return std::string(json.data(), json.size());
}

} // namespace service::certificate_issuance
