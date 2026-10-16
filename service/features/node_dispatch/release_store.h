#pragma once
#include <ruvia/core/task.h>
#include <ruvia/web/db/db_transaction.h>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "service/features/background/queued_work.h"

namespace service::node_dispatch {
struct release_allocation {
    std::string id_;
    std::int64_t generation_;
    std::string access_domain_;
    bool enabled_;
};
inline ruvia::task<release_allocation> allocate_cluster_release(ruvia::db_transaction& transaction,
    std::string_view tenant_id, std::string_view cluster_id, std::int64_t schema_version) {
    const auto cluster_rows = co_await transaction.query(
        "UPDATE sys_cluster cluster SET release_generation = release_generation + 1, updated_at = "
        "NOW() FROM sys_dns_zone zone WHERE cluster.tenant_id = $1 AND cluster.id = $2 AND "
        "cluster.deleted_at IS NULL AND zone.tenant_id = cluster.tenant_id AND zone.id = "
        "cluster.dns_zone_id AND zone.deleted_at IS NULL RETURNING cluster.release_generation, "
        "cluster.hostname_prefix || '.' || zone.domain, cluster.status = 'enabled'",
        tenant_id, cluster_id);
    if (cluster_rows.empty()) {
        throw std::runtime_error("cluster release target does not exist");
    }
    const auto generation = cluster_rows.front()[0].as<std::int64_t>().value();
    const auto release_rows = co_await transaction.query(
        "INSERT INTO sys_cluster_release (tenant_id, cluster_id, generation, schema_version, "
        "status, created_at) VALUES ($1, $2, $3, $4, 'building', NOW()) RETURNING id",
        tenant_id, cluster_id, generation,
        schema_version);
    if (release_rows.empty()) {
        throw std::runtime_error("cluster release identity could not be allocated");
    }
    const auto release_id = std::string(release_rows.front()[0].value().value());
    co_return release_allocation{release_id, generation,
        std::string(cluster_rows.front()[1].value().value()),
        cluster_rows.front()[2].as<bool>().value()};
}

inline ruvia::task<void> link_release_object(ruvia::db_transaction& transaction,
    std::string_view tenant_id, std::string_view release_id, std::int64_t position, std::string_view digest) {
    (void)co_await transaction.execute(
        "INSERT INTO sys_cluster_release_object (tenant_id, release_id, position, "
        "object_digest) VALUES ($1, $2, $3, $4)",
        tenant_id, release_id, position, digest);
    co_return;
}

// Activates a built release: one pending target per eligible node. The node
// task of every new target and of every node whose pending or failed target
// of an earlier release is superseded changes.
inline ruvia::task<void> activate_cluster_release(ruvia::db_transaction& transaction,
    std::string_view tenant_id, std::string_view cluster_id, std::string_view release_id,
    std::string_view manifest_digest, std::string_view manifest_envelope, service::background::queued_work& work) {
    const auto targets = co_await transaction.query(
        "INSERT INTO sys_node_release_target (tenant_id, release_id, node_id, status, updated_at) "
        "SELECT node.tenant_id, $3, node.id, 'pending', NOW() FROM sys_node node WHERE "
        "node.tenant_id = $1 AND node.cluster_id = $2 AND node.registration_status = 'registered' "
        "AND node.status = 'enabled' AND node.agent_id IS NOT NULL AND "
        "node.node_secret_hash IS NOT NULL AND node.node_secret_envelope IS NOT NULL AND "
        "node.deleted_at IS NULL RETURNING node_id::text",
        tenant_id, cluster_id, release_id);
    const auto target_count = static_cast<std::int64_t>(targets.size());
    for (const auto& row : targets) {
        work.changed(row[0].value().value_or(""));
    }
    const auto superseded = co_await transaction.query(
        "SELECT DISTINCT target.node_id::text FROM sys_node_release_target target INNER JOIN sys_cluster_release "
        "release ON release.tenant_id = target.tenant_id AND release.id = target.release_id WHERE "
        "target.tenant_id = $1 AND release.cluster_id = $2 AND release.id <> $3 AND target.status IN "
        "('pending', 'failed')",
        tenant_id, cluster_id, release_id);
    for (const auto& row : superseded) {
        work.changed(row[0].value().value_or(""));
    }
    (void)co_await transaction.execute(
        "UPDATE sys_cluster_release SET manifest_digest = $3, manifest_envelope = $4, status = "
        "'active', target_node_count = $5, activated_at = NOW() WHERE tenant_id = $1 AND id = $2 "
        "AND status = 'building'",
        tenant_id, release_id, manifest_digest, std::string_view(manifest_envelope),
        target_count);
    (void)co_await transaction.execute(
        "UPDATE sys_cluster_release SET status = 'superseded' WHERE tenant_id = $1 AND cluster_id "
        "= $2 AND id <> $3 AND status = 'active'",
        tenant_id, cluster_id, release_id);
    (void)co_await transaction.execute(
        "UPDATE sys_cluster SET current_release_id = $3, updated_at = NOW() WHERE tenant_id = $1 "
        "AND id = $2",
        tenant_id, cluster_id, release_id);
    (void)co_await transaction.execute(
        "UPDATE sys_node SET desired_release_id = $3, updated_at = NOW() WHERE tenant_id = $1 AND "
        "cluster_id = $2 AND registration_status = 'registered' AND agent_id IS NOT NULL AND "
        "node_secret_hash IS NOT NULL AND node_secret_envelope IS NOT NULL AND deleted_at IS NULL",
        tenant_id, cluster_id, release_id);

    co_return;
}

// The release a cluster's nodes are directed to.
struct current_release {
    std::string id_;
    std::optional<std::int64_t> schema_version_;
};

// Locks the cluster FOR UPDATE; `std::nullopt` while it has no release.
inline ruvia::task<std::optional<current_release>> lock_current_release(
    ruvia::db_transaction& transaction, std::string_view tenant_id, std::string_view cluster_id) {
    const auto rows = co_await transaction.query(
        "SELECT cluster.current_release_id, release.schema_version FROM sys_cluster cluster LEFT "
        "JOIN sys_cluster_release release ON release.tenant_id = cluster.tenant_id AND release.id "
        "= cluster.current_release_id WHERE cluster.tenant_id = $1 AND cluster.id = $2 AND "
        "cluster.deleted_at IS NULL LIMIT 1 FOR UPDATE OF cluster",
        tenant_id, cluster_id);
    if (rows.empty()) {
        throw std::runtime_error("cluster release target does not exist");
    }
    const auto id = rows.front()[0].value();
    if (!id) {
        co_return std::nullopt;
    }
    co_return current_release{std::string(*id), rows.front()[1].as<std::int64_t>()};
}

inline ruvia::task<std::vector<std::string>> find_certificate_consumer_clusters(
    ruvia::db_transaction& transaction, std::string_view tenant_id, std::string_view certificate_id) {
    const auto rows = co_await transaction.query(
        "SELECT DISTINCT website.cluster_id FROM sys_certificate certificate INNER JOIN "
        "sys_website_certificate_binding binding ON binding.tenant_id = certificate.tenant_id AND "
        "binding.certificate_id = certificate.id INNER JOIN sys_website website ON "
        "website.tenant_id = binding.tenant_id AND website.id = binding.website_id WHERE "
        "certificate.tenant_id = $1 AND certificate.id = $2 AND website.deleted_at IS NULL ORDER "
        "BY website.cluster_id",
        tenant_id, certificate_id);
    std::vector<std::string> ids;
    ids.reserve(rows.size());
    for (const auto& row : rows) {
        ids.emplace_back(row[0].value().value());
    }
    co_return ids;
}

}  // namespace service::node_dispatch
