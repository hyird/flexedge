#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <ruvia/web/Model.h>
#include <ruvia/web/ModelJson.h>

namespace service::certificate_issuance {

RUVIA_REQUEST_MODEL(CertificateConfigInput,
                    RUVIA_OPTIONAL_FIELD_NAME("auto_renew", autoRenew, ruvia::Bool));
RUVIA_RESPONSE_MODEL(CertificateConfigOutput,
                     RUVIA_REQUIRED_FIELD_NAME("auto_renew", autoRenew, ruvia::Bool));

RUVIA_REQUEST_MODEL(CertificateProviderAccessKeyInput,
                    RUVIA_OPTIONAL_FIELD(envelope, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(hint, ruvia::String));
RUVIA_REQUEST_MODEL(CertificateProviderConfigInput,
                    RUVIA_OPTIONAL_FIELD_NAME("credential_mode", credentialMode, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("account_email", accountEmail, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("access_key", accessKey,
                                              CertificateProviderAccessKeyInput));
RUVIA_RESPONSE_MODEL(CertificateProviderAccessKeyOutput,
                     RUVIA_REQUIRED_FIELD(envelope, ruvia::String),
                     RUVIA_REQUIRED_FIELD(hint, ruvia::String));
RUVIA_RESPONSE_MODEL(CertificateProviderConfigOutput,
                     RUVIA_REQUIRED_FIELD_NAME("credential_mode", credentialMode, ruvia::String),
                     RUVIA_OPTIONAL_FIELD_NAME("account_email", accountEmail, ruvia::String,
                                               RUVIA_OMIT_EMPTY),
                     RUVIA_OPTIONAL_FIELD_NAME("access_key", accessKey,
                                               CertificateProviderAccessKeyOutput,
                                               RUVIA_OMIT_EMPTY));

struct CertificateConfigData final {
    bool autoRenew;
};

[[nodiscard]] inline std::optional<CertificateConfigData>
normalize(const CertificateConfigInput& input) {
    const auto& autoRenew = input.get<"autoRenew">();
    if (!autoRenew) {
        return std::nullopt;
    }
    return CertificateConfigData{.autoRenew = autoRenew->value};
}

inline bool complete(const CertificateConfigInput& config) { return normalize(config).has_value(); }

[[nodiscard]] inline std::optional<CertificateConfigData>
parseConfigStored(std::string_view json, ruvia::ModelParseOptions options = {}) {
    const std::optional<CertificateConfigInput> input =
        ruvia::fromJson<CertificateConfigInput>(json, options);
    return input ? normalize(*input) : std::nullopt;
}

[[nodiscard]] inline CertificateConfigOutput toOutput(const CertificateConfigData& input,
                                                      ruvia::ModelOptions options = {}) {
    CertificateConfigOutput output(options);
    output.set<"autoRenew">(input.autoRenew);
    return output;
}

} // namespace service::certificate_issuance
