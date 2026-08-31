#pragma once

#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <ruvia/web/Model.h>
#include <ruvia/web/ModelJson.h>

namespace service::dns {

struct DnsProviderZoneRuntimeData final {
    std::string domain;
    std::string status;
};

RUVIA_REQUEST_MODEL(ProviderZoneRuntimeInput, RUVIA_OPTIONAL_FIELD(domain, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(status, ruvia::String));
RUVIA_REQUEST_MODEL(DnsProviderRuntimeInput,
                    RUVIA_OPTIONAL_FIELD(zones, ruvia::Array<ProviderZoneRuntimeInput>));
RUVIA_RESPONSE_MODEL(ProviderZoneRuntimeOutput, RUVIA_REQUIRED_FIELD(domain, ruvia::String),
                     RUVIA_REQUIRED_FIELD(status, ruvia::String));
RUVIA_RESPONSE_MODEL(DnsProviderRuntimeOutput,
                     RUVIA_REQUIRED_FIELD(zones, ruvia::Array<ProviderZoneRuntimeOutput>));

inline std::string serializeDnsProviderRuntime(const std::vector<DnsProviderZoneRuntimeData>& zones,
                                               std::pmr::memory_resource* resource) {
    DnsProviderRuntimeOutput runtime({.resource = resource});
    auto& output = runtime.ensure<"zones">();
    for (const auto& zone : zones) {
        auto& item = output.emplace_back(ruvia::ModelOptions{.resource = resource});
        item.set<"domain">(zone.domain);
        item.set<"status">(zone.status);
    }
    const auto json = ruvia::toJson(runtime, {.resource = resource});
    return std::string(json.data(), json.size());
}

inline std::vector<DnsProviderZoneRuntimeData>
parseDnsProviderRuntime(std::string_view json, std::pmr::memory_resource* resource) {
    const std::optional<DnsProviderRuntimeInput> runtime =
        ruvia::fromJson<DnsProviderRuntimeInput>(json, {.resource = resource});
    if (!runtime) {
        throw std::runtime_error("stored DNS provider runtime is incomplete");
    }
    const auto& zoneInput = runtime->get<"zones">();
    if (!zoneInput) {
        throw std::runtime_error("stored DNS provider runtime is incomplete");
    }
    std::vector<DnsProviderZoneRuntimeData> zones;
    zones.reserve(zoneInput->size());
    for (const auto& zone : *zoneInput) {
        const auto& domain = zone.get<"domain">();
        const auto& status = zone.get<"status">();
        if (!domain || !status) {
            throw std::runtime_error("stored DNS provider zone runtime is incomplete");
        }
        zones.push_back(
            {.domain = std::string(domain->view()), .status = std::string(status->view())});
    }
    return zones;
}

} // namespace service::dns
