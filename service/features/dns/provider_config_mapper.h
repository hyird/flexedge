#pragma once

#include <ruvia/web/model_json.h>

#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "service/features/dns/provider_config_model.h"
#include "service/features/dns/provider_config_transport.h"

namespace service::dns {

[[nodiscard]] inline std::optional<dns_provider_config_data>
normalize(const dns_provider_config_input& input) {
    const auto& credential = input.get<"credential">();
    if (!credential) {
        return std::nullopt;
    }
    const auto& envelope = credential->get<"envelope">();
    const auto& hint = credential->get<"hint">();
    if (!envelope || !hint) {
        return std::nullopt;
    }
    return dns_provider_config_data{.credential_envelope_ = std::string(envelope->view()),
        .credential_hint_ = std::string(hint->view())};
}

inline dns_provider_config_data
parse_dns_provider_config(std::string_view json,
    std::pmr::memory_resource* resource = std::pmr::get_default_resource()) {
    const std::optional<dns_provider_config_input> config =
        ruvia::from_json<dns_provider_config_input>(json, {.resource_ = resource});
    if (!config) {
        throw std::runtime_error("stored DNS provider config is incomplete");
    }
    const auto normalized = normalize(*config);
    if (!normalized) {
        throw std::runtime_error("stored DNS provider config is incomplete");
    }
    return *normalized;
}

[[nodiscard]] inline dns_provider_config_output to_output(const dns_provider_config_data& input,
    ruvia::model_options options = {}) {
    dns_provider_config_output output(options);
    auto& credential = output.ensure<"credential">();
    credential.set<"envelope">(input.credential_envelope_);
    credential.set<"hint">(input.credential_hint_);
    return output;
}

inline std::string
serialize_dns_provider_config(std::string_view envelope, std::string_view hint,
    std::pmr::memory_resource* resource = std::pmr::get_default_resource()) {
    const auto config = to_output(dns_provider_config_data{.credential_envelope_ = std::string(envelope),
                                      .credential_hint_ = std::string(hint)},
        {.resource_ = resource});
    const auto json = ruvia::to_json(config, {.resource_ = resource});
    return std::string(json.data(), json.size());
}

}  // namespace service::dns
