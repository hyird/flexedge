#pragma once
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <ruvia/core/Task.h>
#include <ruvia/web/db/DbTransaction.h>
#include "service/features/certificate_material/model.h"
#include "service/features/node_dispatch/deployment_source.h"

namespace service::node_dispatch {

inline ruvia::Task<ClusterDeploymentSource>
loadClusterDeploymentSource(ruvia::DbTransaction& transaction, std::string tenantId,
                            std::string clusterId, std::string accessDomain, bool clusterEnabled) {
    ClusterDeploymentSource result{.clusterId = std::move(clusterId),
                                   .tenantId = std::move(tenantId),
                                   .accessDomain = std::move(accessDomain),
                                   .enabled = clusterEnabled,
                                   .websites = {},
                                   .certificatesByWebsite = {}};
    const auto websiteRows = co_await transaction.query(
        "SELECT id, revision, status = 'enabled', config::text FROM sys_website WHERE tenant_id = "
        "$1 AND cluster_id = $2 AND deleted_at IS NULL ORDER BY sort ASC",
        result.tenantId, result.clusterId);
    result.websites.reserve(websiteRows.size());
    for (const auto& row : websiteRows) {
        result.websites.push_back({
            .id = std::string(row[0].value().value()),
            .revision = row[1].as<std::int64_t>().value(),
            .enabled = row[2].as<bool>().value(),
            .configJson = std::string(row[3].value().value()),
        });
    }

    const auto certificateRows = co_await transaction.query(
        "SELECT binding.website_id, certificate.id, certificate.subject_alt_names[1], "
        "certificate.subject_alt_names[2], "
        "certificate.material::text FROM sys_website_certificate_binding binding INNER JOIN "
        "sys_website website ON website.tenant_id = binding.tenant_id AND website.id = "
        "binding.website_id INNER JOIN sys_certificate certificate ON certificate.tenant_id = "
        "binding.tenant_id AND certificate.id = binding.certificate_id WHERE "
        "binding.tenant_id = $1 AND website.cluster_id = $2 AND website.deleted_at IS NULL AND "
        "certificate.issued_revision > 0 AND certificate.expires_at > NOW() AND "
        "certificate.deleted_at IS NULL ORDER BY website.sort ASC, binding.position ASC",
        result.tenantId, result.clusterId);
    for (const auto& row : certificateRows) {
        const auto material =
            service::certificate_material::parseStored(row[4].value().value());
        if (!material || !material->certificateChainPem || !material->privateKeyEnvelope) {
            throw std::runtime_error("certificate release material is invalid");
        }
        std::vector<std::string> domains;
        for (const auto index : {2U, 3U}) {
            if (const auto name = row[index].value(); name && !name->empty()) {
                domains.emplace_back(*name);
            }
        }
        result.certificatesByWebsite[std::string(row[0].value().value())].push_back({
            .id = std::string(row[1].value().value()),
            .domains = std::move(domains),
            .certificateChainPem = *material->certificateChainPem,
            .privateKeyEnvelope = *material->privateKeyEnvelope,
        });
    }
    co_return result;
}

} // namespace service::node_dispatch
