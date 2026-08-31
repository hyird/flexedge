#pragma once

#include <optional>
#include <string>

#include <ruvia/web/Model.h>

namespace service::cluster {

RUVIA_REQUEST_MODEL(SaveClusterBody, RUVIA_OPTIONAL_FIELD(name, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("dns_zone_id", dnsZoneId, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("hostname_prefix", hostnamePrefix, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(status, ruvia::String));

struct ClusterSaveData final {
    std::string name;
    std::string dnsZoneId;
    std::string hostnamePrefix;
    std::string status;
};

[[nodiscard]] inline std::optional<ClusterSaveData> normalize(const SaveClusterBody& input) {
    const auto& name = input.get<"name">();
    const auto& dnsZoneId = input.get<"dnsZoneId">();
    const auto& hostnamePrefix = input.get<"hostnamePrefix">();
    const auto& status = input.get<"status">();
    if (!name || !dnsZoneId || !hostnamePrefix || !status) {
        return std::nullopt;
    }
    return ClusterSaveData{.name = std::string(name->view()),
                           .dnsZoneId = std::string(dnsZoneId->view()),
                           .hostnamePrefix = std::string(hostnamePrefix->view()),
                           .status = std::string(status->view())};
}

RUVIA_RESPONSE_MODEL(ClusterDto, RUVIA_REQUIRED_FIELD(id, ruvia::String),
                     RUVIA_REQUIRED_FIELD(name, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("dns_zone_id", dnsZoneId, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("dns_zone_domain", dnsZoneDomain, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("dns_provider_name", dnsProviderName, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("hostname_prefix", hostnamePrefix, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("access_domain", accessDomain, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("node_count", nodeCount, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD_NAME("online_node_count", onlineNodeCount, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD(status, ruvia::String),
                     RUVIA_REQUIRED_FIELD(revision, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD_NAME("created_at", createdAt, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("updated_at", updatedAt, ruvia::String));

RUVIA_RESPONSE_MODEL(ClusterPageDataDto, RUVIA_REQUIRED_FIELD(list, ruvia::Array<ClusterDto>),
                     RUVIA_REQUIRED_FIELD(total, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD(page, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD_NAME("page_size", pageSize, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD_NAME("total_pages", totalPages, ruvia::Int64));

RUVIA_RESPONSE_MODEL(ClusterPageResponse, RUVIA_REQUIRED_FIELD(code, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD(message, ruvia::String),
                     RUVIA_REQUIRED_FIELD(data, ClusterPageDataDto));

} // namespace service::cluster
