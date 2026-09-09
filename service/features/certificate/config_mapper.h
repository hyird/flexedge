#pragma once

#include <optional>
#include <string_view>
#include <ruvia/web/ModelJson.h>

#include "service/features/certificate/model.h"
#include "service/features/certificate/config_transport.h"

namespace service::certificate_issuance {

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
