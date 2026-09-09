#pragma once
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <ruvia/core/Task.h>
#include <ruvia/web/db/DbTransaction.h>

namespace service::node_dispatch {
struct ReleaseAllocation {
    std::string id;
    std::int64_t generation;
    std::string accessDomain;
    bool enabled;
};
inline ruvia::Task<ReleaseAllocation> allocateClusterRelease(ruvia::DbTransaction& transaction,
    std::string_view tenantId, std::string_view clusterId, std::int64_t schemaVersion) {
    const auto clusterRows = co_await transaction.query(
        "UPDATE sys_cluster cluster SET release_generation = release_generation + 1, updated_at = "
        "NOW() FROM sys_dns_zone zone WHERE cluster.tenant_id = $1 AND cluster.id = $2 AND "
        "cluster.deleted_at IS NULL AND zone.tenant_id = cluster.tenant_id AND zone.id = "
        "cluster.dns_zone_id AND zone.deleted_at IS NULL RETURNING cluster.release_generation, "
        "cluster.hostname_prefix || '.' || zone.domain, cluster.status = 'enabled'",
        tenantId, clusterId);
    if (clusterRows.empty()) {
        throw std::runtime_error("cluster release target does not exist");
    }
    const auto generation = clusterRows.front()[0].as<std::int64_t>().value();
    const auto releaseRows = co_await transaction.query(
        "INSERT INTO sys_cluster_release (tenant_id, cluster_id, generation, schema_version, "
        "status, created_at) VALUES ($1, $2, $3, $4, 'building', NOW()) RETURNING id",
        tenantId, clusterId, generation,
        schemaVersion);
    if (releaseRows.empty()) {
        throw std::runtime_error("cluster release identity could not be allocated");
    }
    const auto releaseId = std::string(releaseRows.front()[0].value().value());
    co_return ReleaseAllocation{releaseId, generation,
        std::string(clusterRows.front()[1].value().value()),
        clusterRows.front()[2].as<bool>().value()};
}

inline ruvia::Task<void> linkReleaseObject(ruvia::DbTransaction& transaction,
    std::string_view tenantId, std::string_view releaseId, std::int64_t position, std::string_view digest) {
        (void)co_await transaction.execute(
            "INSERT INTO sys_cluster_release_object (tenant_id, release_id, position, "
            "object_digest) VALUES ($1, $2, $3, $4)",
            tenantId, releaseId, position, digest);
    co_return;
}

inline ruvia::Task<void> activateClusterRelease(ruvia::DbTransaction& transaction,
    std::string_view tenantId, std::string_view clusterId, std::string_view releaseId,
    std::string_view manifestDigest, std::string_view manifestEnvelope) {
    const auto targets = co_await transaction.execute(
        "INSERT INTO sys_node_release_target (tenant_id, release_id, node_id, status, updated_at) "
        "SELECT node.tenant_id, $3, node.id, 'pending', NOW() FROM sys_node node WHERE "
        "node.tenant_id = $1 AND node.cluster_id = $2 AND node.registration_status = 'registered' "
        "AND node.status = 'enabled' AND node.agent_id IS NOT NULL AND "
        "node.node_secret_hash IS NOT NULL AND node.node_secret_envelope IS NOT NULL AND "
        "node.deleted_at IS NULL",
        tenantId, clusterId, releaseId);
    const auto targetCount = static_cast<std::int64_t>(targets.affectedRows());
    (void)co_await transaction.execute(
        "UPDATE sys_cluster_release SET manifest_digest = $3, manifest_envelope = $4, status = "
        "'active', target_node_count = $5, activated_at = NOW() WHERE tenant_id = $1 AND id = $2 "
        "AND status = 'building'",
        tenantId, releaseId, manifestDigest, std::string_view(manifestEnvelope),
        targetCount);
    (void)co_await transaction.execute(
        "UPDATE sys_cluster_release SET status = 'superseded' WHERE tenant_id = $1 AND cluster_id "
        "= $2 AND id <> $3 AND status = 'active'",
        tenantId, clusterId, releaseId);
    (void)co_await transaction.execute(
        "UPDATE sys_cluster SET current_release_id = $3, updated_at = NOW() WHERE tenant_id = $1 "
        "AND id = $2",
        tenantId, clusterId, releaseId);
    (void)co_await transaction.execute(
        "UPDATE sys_node SET desired_release_id = $3, updated_at = NOW() WHERE tenant_id = $1 AND "
        "cluster_id = $2 AND registration_status = 'registered' AND agent_id IS NOT NULL AND "
        "node_secret_hash IS NOT NULL AND node_secret_envelope IS NOT NULL AND deleted_at IS NULL",
        tenantId, clusterId, releaseId);

    co_return;
}

inline ruvia::Task<std::optional<std::int64_t>> lockCurrentReleaseSchemaVersion(
    ruvia::DbTransaction& transaction, std::string_view tenantId, std::string_view clusterId) {
    const auto rows = co_await transaction.query(
        "SELECT cluster.current_release_id, release.schema_version FROM sys_cluster cluster LEFT "
        "JOIN sys_cluster_release release ON release.tenant_id = cluster.tenant_id AND release.id "
        "= cluster.current_release_id WHERE cluster.tenant_id = $1 AND cluster.id = $2 AND "
        "cluster.deleted_at IS NULL LIMIT 1 FOR UPDATE OF cluster",
        tenantId, clusterId);
    if (rows.empty()) {
        throw std::runtime_error("cluster release target does not exist");
    }
    if (!rows.front()[0].value()) co_return std::nullopt;
    co_return rows.front()[1].as<std::int64_t>();
}

inline ruvia::Task<std::vector<std::string>> findCertificateConsumerClusters(
    ruvia::DbTransaction& transaction, std::string_view tenantId, std::string_view certificateId) {
    const auto rows = co_await transaction.query(
        "SELECT DISTINCT website.cluster_id FROM sys_certificate certificate INNER JOIN "
        "sys_website_certificate_binding binding ON binding.tenant_id = certificate.tenant_id AND "
        "binding.certificate_id = certificate.id INNER JOIN sys_website website ON "
        "website.tenant_id = binding.tenant_id AND website.id = binding.website_id WHERE "
        "certificate.tenant_id = $1 AND certificate.id = $2 AND website.deleted_at IS NULL ORDER "
        "BY website.cluster_id",
        tenantId, certificateId);
    std::vector<std::string> ids;
    ids.reserve(rows.size());
    for (const auto& row : rows) ids.emplace_back(row[0].value().value());
    co_return ids;
}

} // namespace service::node_dispatch
