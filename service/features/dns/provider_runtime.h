#pragma once

#include <ruvia/web/model_json.h>

#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/web/model.h"

namespace service::dns {

struct dns_provider_zone_runtime_data final {
    std::string domain_;
    std::string status_;
};

RUVIA_MODEL(provider_zone_runtime_input, RUVIA_OPTIONAL_FIELD(domain, ruvia::string),
    RUVIA_OPTIONAL_FIELD(status, ruvia::string));
RUVIA_MODEL(dns_provider_runtime_input,
    RUVIA_OPTIONAL_FIELD(zones, ruvia::array<provider_zone_runtime_input>));
RUVIA_MODEL(provider_zone_runtime_output, RUVIA_REQUIRED_FIELD(domain, ruvia::string),
    RUVIA_REQUIRED_FIELD(status, ruvia::string));
RUVIA_MODEL(dns_provider_runtime_output,
    RUVIA_REQUIRED_FIELD(zones, ruvia::array<provider_zone_runtime_output>));

inline std::string serialize_dns_provider_runtime(const std::vector<dns_provider_zone_runtime_data>& zones,
    std::pmr::memory_resource* resource) {
    dns_provider_runtime_output runtime({.resource_ = resource});
    auto& output = runtime.ensure<"zones">();
    for (const auto& zone : zones) {
        auto& item = output.emplace_back(ruvia::model_options{.resource_ = resource});
        item.set<"domain">(zone.domain_);
        item.set<"status">(zone.status_);
    }
    const auto json = ruvia::to_json(runtime, {.resource_ = resource});
    return std::string(json.data(), json.size());
}

inline std::vector<dns_provider_zone_runtime_data>
parse_dns_provider_runtime(std::string_view json, std::pmr::memory_resource* resource) {
    const std::optional<dns_provider_runtime_input> runtime =
        ruvia::from_json<dns_provider_runtime_input>(json, {.resource_ = resource});
    if (!runtime) {
        throw std::runtime_error("stored DNS provider runtime is incomplete");
    }
    const auto& zone_input = runtime->get<"zones">();
    if (!zone_input) {
        throw std::runtime_error("stored DNS provider runtime is incomplete");
    }
    std::vector<dns_provider_zone_runtime_data> zones;
    zones.reserve(zone_input->size());
    for (const auto& zone : *zone_input) {
        const auto& domain = zone.get<"domain">();
        const auto& status = zone.get<"status">();
        if (!domain || !status) {
            throw std::runtime_error("stored DNS provider zone runtime is incomplete");
        }
        zones.push_back(
            {.domain_ = std::string(domain->view()), .status_ = std::string(status->view())});
    }
    return zones;
}

}  // namespace service::dns
