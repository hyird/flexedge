#pragma once

#include "service/features/certificate/acme_types.h"
#include "service/features/certificate/provider_config_mapper.h"
#include "service/utils/secret.h"

namespace service::certificate_issuance {

inline std::string
serialize_certificate_provider_runtime(const std::optional<certificate_provider_runtime_data>& runtime,
    const std::optional<eab_credentials>& eab,
    std::pmr::memory_resource* resource) {
    certificate_provider_runtime_output output =
        runtime ? to_output(*runtime, {.resource_ = resource})
                : certificate_provider_runtime_output({.resource_ = resource});
    if (eab) {
        auto& eab_output = output.ensure<"eab">();
        eab_output.set<"kid">(eab->key_id_);
        eab_output.set<"hmac_key_envelope">(service::utils::seal_secret(eab->hmac_key_.view()));
    }
    const auto json = ruvia::to_json(output, {.resource_ = resource});
    return std::string(json.data(), json.size());
}
}  // namespace service::certificate_issuance
