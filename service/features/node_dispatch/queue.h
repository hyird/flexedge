#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

#include <ruvia/core/Task.h>
#include <ruvia/web/db/Db.h>
#include <ruvia/web/db/DbTransaction.h>

#include "node/proto/artifact.h"
#include "service/features/certificate_material/model.h"
#include "service/features/node_dispatch/protocol.h"
#include "service/utils/secret.h"
#include "service/utils/sensitive_string.h"

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
            .id = std::string(row[0].value().value_or("")),
            .revision = row[1].as<std::int64_t>().value_or(1),
            .enabled = row[2].as<bool>().value_or(false),
            .configJson = std::string(row[3].value().value_or("{}")),
        });
    }

    const auto certificateRows = co_await transaction.query(
        "SELECT binding.website_id, certificate.id, certificate.domain, "
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
            service::certificate_material::parseStored(row[3].value().value_or("{}"));
        if (!material || !material->certificateChainPem || !material->privateKeyEnvelope) {
            throw std::runtime_error("certificate release material is invalid");
        }
        result.certificatesByWebsite[std::string(row[0].value().value_or(""))].push_back({
            .id = std::string(row[1].value().value_or("")),
            .domain = std::string(row[2].value().value_or("")),
            .certificateChainPem = *material->certificateChainPem,
            .privateKeyEnvelope = *material->privateKeyEnvelope,
        });
    }
    co_return result;
}

inline ruvia::Task<void> persistObject(ruvia::DbTransaction& transaction, std::string_view tenantId,
                                       const flexedge::node::v2::DeliveryObject& object) {
    if (!object.has_content() ||
        flexedge::node::artifactDigest(object.content()) != object.digest_sha256()) {
        throw std::runtime_error("delivery object digest does not match payload");
    }
    if (!object.content().has_website() && !object.content().has_certificate()) {
        throw std::runtime_error("delivery object has no supported payload");
    }
    const auto kind = object.content().has_website() ? std::string_view{"website"}
                                                     : std::string_view{"certificate"};
    const auto plaintext = flexedge::node::serializeArtifact(object.content());
    const auto envelope = service::utils::sealSecret(plaintext);
    const auto inserted = co_await transaction.execute(
        "INSERT INTO sys_delivery_object (tenant_id, digest_sha256, kind, payload_envelope, "
        "created_at) VALUES ($1, $2, $3, $4, NOW()) ON CONFLICT (tenant_id, digest_sha256) DO "
        "NOTHING",
        tenantId, object.digest_sha256(), kind, std::string_view(envelope));
    if (inserted.affectedRows() == 0) {
        const auto rows = co_await transaction.query(
            "SELECT kind, payload_envelope FROM sys_delivery_object WHERE tenant_id = $1 AND "
            "digest_sha256 = $2 LIMIT 1",
            tenantId, object.digest_sha256());
        if (rows.size() != 1 || rows.front()[0].value().value_or("") != kind) {
            throw std::runtime_error("content-addressed delivery object conflicts with storage");
        }
        service::utils::SensitiveString stored(
            service::utils::openSecret(rows.front()[1].value().value_or("")));
        if (stored.view() != plaintext) {
            throw std::runtime_error(
                "content-addressed delivery object failed storage verification");
        }
    }
    co_return;
}

inline ruvia::Task<void> publishClusterRelease(ruvia::DbTransaction& transaction,
                                               std::string_view tenantId,
                                               std::string_view clusterId) {
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
    const auto generation = clusterRows.front()[0].as<std::int64_t>().value_or(1);
    const auto releaseRows = co_await transaction.query(
        "INSERT INTO sys_cluster_release (tenant_id, cluster_id, generation, schema_version, "
        "status, created_at) VALUES ($1, $2, $3, $4, 'building', NOW()) RETURNING id",
        tenantId, clusterId, generation,
        static_cast<std::int64_t>(flexedge::node::kClusterReleaseSchemaVersion));
    if (releaseRows.empty()) {
        throw std::runtime_error("cluster release identity could not be allocated");
    }
    const auto releaseId = std::string(releaseRows.front()[0].value().value_or(""));
    const auto deployment = co_await loadClusterDeploymentSource(
        transaction, std::string(tenantId), std::string(clusterId),
        std::string(clusterRows.front()[1].value().value_or("")),
        clusterRows.front()[2].as<bool>().value_or(false));
    const auto artifact = buildClusterRelease(releaseId, generation, deployment);

    std::int64_t position{};
    for (const auto& object : artifact.objects) {
        co_await persistObject(transaction, tenantId, object);
        (void)co_await transaction.execute(
            "INSERT INTO sys_cluster_release_object (tenant_id, release_id, position, "
            "object_digest) VALUES ($1, $2, $3, $4)",
            tenantId, releaseId, position++, object.digest_sha256());
    }
    const auto manifestBytes = flexedge::node::serializeArtifact(artifact.manifest);
    const auto manifestEnvelope = service::utils::sealSecret(manifestBytes);
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
        tenantId, releaseId, artifact.manifest.digest_sha256(), std::string_view(manifestEnvelope),
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

inline ruvia::Task<void> ensureClusterRelease(ruvia::DbTransaction& transaction,
                                              std::string_view tenantId,
                                              std::string_view clusterId) {
    const auto rows = co_await transaction.query(
        "SELECT cluster.current_release_id, release.schema_version FROM sys_cluster cluster LEFT "
        "JOIN sys_cluster_release release ON release.tenant_id = cluster.tenant_id AND release.id "
        "= cluster.current_release_id WHERE cluster.tenant_id = $1 AND cluster.id = $2 AND "
        "cluster.deleted_at IS NULL LIMIT 1 FOR UPDATE OF cluster",
        tenantId, clusterId);
    if (rows.empty()) {
        throw std::runtime_error("cluster release target does not exist");
    }
    if (!rows.front()[0].value() ||
        rows.front()[1].as<std::int64_t>().value_or(0) !=
            static_cast<std::int64_t>(flexedge::node::kClusterReleaseSchemaVersion)) {
        co_await publishClusterRelease(transaction, tenantId, clusterId);
    }
    co_return;
}

inline ruvia::Task<void> enqueueCertificateConsumers(ruvia::DbTransaction& transaction,
                                                     std::string_view tenantId,
                                                     std::string_view certificateId) {
    const auto rows = co_await transaction.query(
        "SELECT DISTINCT website.cluster_id FROM sys_certificate certificate INNER JOIN "
        "sys_website_certificate_binding binding ON binding.tenant_id = certificate.tenant_id AND "
        "binding.certificate_id = certificate.id INNER JOIN sys_website website ON "
        "website.tenant_id = binding.tenant_id AND website.id = binding.website_id WHERE "
        "certificate.tenant_id = $1 AND certificate.id = $2 AND website.deleted_at IS NULL ORDER "
        "BY website.cluster_id",
        tenantId, certificateId);
    for (const auto& row : rows) {
        co_await publishClusterRelease(transaction, tenantId, row[0].value().value_or(""));
    }
    co_return;
}

} // namespace service::node_dispatch
