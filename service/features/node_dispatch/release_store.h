#pragma once
#include <ruvia/core/task.h>
#include <ruvia/web/db/db_transaction.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "service/features/operation/work_origin.h"

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

// Activates a built release: one target per eligible node, each owned by a
// node publish subtask of `origin` (none for unowned work). Pending or failed
// targets of earlier releases are superseded: their subtasks merge into the
// new subtask of the same node, or are cancelled when the node is no longer
// a target.
inline ruvia::task<void> activate_cluster_release(ruvia::db_transaction& transaction,
    std::string_view tenant_id, std::string_view cluster_id, std::string_view release_id, std::int64_t generation,
    std::string_view manifest_digest, std::string_view manifest_envelope, service::operation::work_origin& origin) {
    const auto targets = co_await transaction.query(
        "INSERT INTO sys_node_release_target (tenant_id, release_id, node_id, status, updated_at) "
        "SELECT node.tenant_id, $3, node.id, 'pending', NOW() FROM sys_node node WHERE "
        "node.tenant_id = $1 AND node.cluster_id = $2 AND node.registration_status = 'registered' "
        "AND node.status = 'enabled' AND node.agent_id IS NOT NULL AND "
        "node.node_secret_hash IS NOT NULL AND node.node_secret_envelope IS NOT NULL AND "
        "node.deleted_at IS NULL RETURNING node_id::text",
        tenant_id, cluster_id, release_id);
    const auto target_count = static_cast<std::int64_t>(targets.size());
    std::vector<std::pair<std::string, std::string>> owners;
    owners.reserve(targets.size());
    for (const auto& row : targets) {
        const auto node_id = std::string(row[0].value().value_or(""));
        const auto subtask = co_await origin.require_subtask(transaction, tenant_id,
            service::operation::subtask_kind::node_publish, "node", node_id, generation);
        if (subtask) {
            (void)co_await transaction.execute(
                "UPDATE sys_node_release_target SET subtask_id = $4::uuid WHERE tenant_id = $1 AND "
                "release_id = $2 AND node_id = $3",
                tenant_id, release_id, std::string_view(node_id), std::string_view(*subtask));
        }
        owners.emplace_back(node_id, subtask.value_or(""));
    }
    const auto superseded = co_await transaction.query(
        "SELECT target.subtask_id::text, target.node_id::text FROM sys_node_release_target target "
        "INNER JOIN sys_cluster_release release ON release.tenant_id = target.tenant_id AND release.id = "
        "target.release_id INNER JOIN sys_operation_subtask subtask ON subtask.id = target.subtask_id WHERE "
        "target.tenant_id = $1 AND release.cluster_id = $2 AND release.id <> $3 AND target.status IN "
        "('pending', 'failed') AND subtask.state = 'open'",
        tenant_id, cluster_id, release_id);
    const auto closed_by = origin.operation_id().value_or("");
    for (const auto& row : superseded) {
        const auto subtask = std::string(row[0].value().value_or(""));
        const auto node_id = row[1].value().value_or("");
        const auto next = std::find_if(owners.begin(), owners.end(),
            [&](const auto& owner) { return owner.first == node_id; });
        if (next != owners.end() && next->second == subtask) {
            continue;
        }
        const auto settled = co_await service::operation::settle_subtask(transaction, tenant_id, subtask,
            next == owners.end() ? service::operation::state_cancelled : service::operation::state_merged,
            next == owners.end() ? std::string_view{} : std::string_view(next->second), closed_by);
        origin.touch(settled.value_or(""));
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

inline ruvia::task<std::optional<std::int64_t>> lock_current_release_schema_version(
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
    if (!rows.front()[0].value()) {
        co_return std::nullopt;
    }
    co_return rows.front()[1].as<std::int64_t>();
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
