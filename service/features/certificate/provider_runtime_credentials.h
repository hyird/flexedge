#pragma once

#include "service/features/certificate/acme_types.h"
#include "service/features/certificate/provider_config_mapper.h"
#include "service/utils/secret.h"

namespace service::certificate_issuance {

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
