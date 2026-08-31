#pragma once

#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <ruvia/web/Model.h>
#include <ruvia/web/ModelJson.h>

namespace service::dns {

RUVIA_REQUEST_MODEL(DnsCredentialConfigInput, RUVIA_OPTIONAL_FIELD(envelope, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(hint, ruvia::String));
RUVIA_REQUEST_MODEL(DnsProviderConfigInput,
                    RUVIA_OPTIONAL_FIELD(credential, DnsCredentialConfigInput));
RUVIA_RESPONSE_MODEL(DnsCredentialConfigOutput, RUVIA_REQUIRED_FIELD(envelope, ruvia::String),
                     RUVIA_REQUIRED_FIELD(hint, ruvia::String));
RUVIA_RESPONSE_MODEL(DnsProviderConfigOutput,
                     RUVIA_REQUIRED_FIELD(credential, DnsCredentialConfigOutput));

struct DnsProviderConfigData final {
    std::string credentialEnvelope;
    std::string credentialHint;
};

[[nodiscard]] inline std::optional<DnsProviderConfigData>
normalize(const DnsProviderConfigInput& input) {
    const auto& credential = input.get<"credential">();
    if (!credential) {
        return std::nullopt;
    }
    const auto& envelope = credential->get<"envelope">();
    const auto& hint = credential->get<"hint">();
    if (!envelope || !hint) {
        return std::nullopt;
    }
    return DnsProviderConfigData{.credentialEnvelope = std::string(envelope->view()),
                                 .credentialHint = std::string(hint->view())};
}

inline DnsProviderConfigData
parseDnsProviderConfig(std::string_view json,
                       std::pmr::memory_resource* resource = std::pmr::get_default_resource()) {
    const std::optional<DnsProviderConfigInput> config =
        ruvia::fromJson<DnsProviderConfigInput>(json, {.resource = resource});
    if (!config) {
        throw std::runtime_error("stored DNS provider config is incomplete");
    }
    const auto normalized = normalize(*config);
    if (!normalized) {
        throw std::runtime_error("stored DNS provider config is incomplete");
    }
    return *normalized;
}

[[nodiscard]] inline DnsProviderConfigOutput toOutput(const DnsProviderConfigData& input,
                                                      ruvia::ModelOptions options = {}) {
    DnsProviderConfigOutput output(options);
    auto& credential = output.ensure<"credential">();
    credential.set<"envelope">(input.credentialEnvelope);
    credential.set<"hint">(input.credentialHint);
    return output;
}

inline std::string
serializeDnsProviderConfig(std::string_view envelope, std::string_view hint,
                           std::pmr::memory_resource* resource = std::pmr::get_default_resource()) {
    const auto config = toOutput(DnsProviderConfigData{.credentialEnvelope = std::string(envelope),
                                                       .credentialHint = std::string(hint)},
                                 {.resource = resource});
    const auto json = ruvia::toJson(config, {.resource = resource});
    return std::string(json.data(), json.size());
}

} // namespace service::dns
