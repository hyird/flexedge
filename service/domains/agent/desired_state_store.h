#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/db/db_transaction.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "service/domains/agent/agent_types.h"

namespace service::agent {

struct desired_state_record final {
    std::int64_t revision_;
    std::string name_;
    bool enabled_;
    std::string config_json_, release_id_, manifest_digest_, manifest_envelope_;
};

inline ruvia::task<std::optional<desired_state_record>> lock_desired_state(
    ruvia::db_transaction& transaction, const agent_principal& principal) {
    const auto rows = co_await transaction.query(
        "SELECT node.node_spec_revision, node.name, node.status = 'enabled', node.config::text, "
        "node.desired_release_id, release.manifest_digest, release.manifest_envelope FROM "
        "sys_node node INNER JOIN sys_cluster_release release ON release.tenant_id = "
        "node.tenant_id AND release.id = node.desired_release_id AND release.cluster_id = node.cluster_id "
        "WHERE node.tenant_id = $1 AND node.id = $2 AND node.cluster_id = $3 AND node.agent_id = $4 AND "
        "node.registration_status = 'registered' AND node.deleted_at IS NULL LIMIT 1 FOR UPDATE OF node",
        principal.tenant_id_, principal.node_id_, principal.cluster_id_, principal.agent_id_);
    if (rows.empty()) {
        co_return std::nullopt;
    }
    const auto& row = rows.front();
    co_return desired_state_record{
        .revision_ = row[0].as<std::int64_t>().value_or(1),
        .name_ = std::string(row[1].value().value_or("")),
        .enabled_ = row[2].as<bool>().value_or(false),
        .config_json_ = std::string(row[3].value().value_or("{}")),
        .release_id_ = std::string(row[4].value().value_or("")),
        .manifest_digest_ = std::string(row[5].value().value_or("")),
        .manifest_envelope_ = std::string(row[6].value().value_or(""))};
}

inline ruvia::task<bool> record_node_spec_digest(ruvia::db_transaction& transaction,
    std::string_view tenant_id, std::string_view node_id, std::int64_t revision,
    std::string_view digest) {
    const auto updated = co_await transaction.execute(
        "UPDATE sys_node SET node_spec_digest = $3, updated_at = NOW() WHERE tenant_id = $1 "
        "AND id = $2 AND node_spec_revision = $4",
        tenant_id, node_id, digest, revision);
    co_return updated.affected_rows() == 1;
}

}  // namespace service::agent
