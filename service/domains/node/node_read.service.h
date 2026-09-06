#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <ruvia/core/Task.h>
#include <ruvia/web/Context.h>
#include <ruvia/web/db/Db.h>

#include "service/common/http.h"
#include "service/domains/node/node.error.h"
#include "service/domains/node/node.mapper.h"
#include "service/domains/node/node.types.h"
#include "service/features/log_ingest/tail.h"
#include "service/utils/secret.h"
#include "service/utils/sensitive_string.h"

namespace service::node {

class NodeReadService final {
  public:
    ruvia::Task<NodePageDataDto> list(ruvia::Context& c, const std::string& tenantId,
                                      std::int64_t page, std::int64_t pageSize, std::int64_t skip,
                                      const std::optional<std::string>& keyword,
                                      const std::optional<std::string>& clusterId,
                                      const std::optional<std::string>& status,
                                      const std::optional<std::string>& registrationStatus,
                                      const std::optional<std::string>& connectionStatus) const {
        std::string where =
            " FROM sys_node node INNER JOIN sys_cluster cluster ON cluster.tenant_id = "
            "node.tenant_id AND cluster.id = node.cluster_id WHERE node.tenant_id = $1 AND "
            "node.deleted_at IS NULL AND cluster.deleted_at IS NULL";
        std::vector<ruvia::DbValue> params{ruvia::DbValue{tenantId}};
        std::optional<std::string> pattern;
        if (keyword) {
            pattern = "%" + service::common::escapeLikePattern(*keyword) + "%";
            where += " AND node.name ILIKE $" + std::to_string(params.size() + 1);
            params.emplace_back(std::string_view(*pattern));
        }
        if (clusterId) {
            where += " AND node.cluster_id = $" + std::to_string(params.size() + 1);
            params.emplace_back(*clusterId);
        }
        if (status) {
            where += " AND node.status = $" + std::to_string(params.size() + 1);
            params.emplace_back(std::string_view(*status));
        }
        if (registrationStatus) {
            where += " AND node.registration_status = $" + std::to_string(params.size() + 1);
            params.emplace_back(std::string_view(*registrationStatus));
        }
        if (connectionStatus) {
            const auto placeholder = "$" + std::to_string(params.size() + 1);
            where +=
                " AND (CASE WHEN node.registration_status <> 'registered' THEN 'unregistered' "
                "WHEN node.status = 'enabled' AND node.last_heartbeat_at >= NOW() - INTERVAL '90 "
                "seconds' THEN 'online' ELSE 'offline' END) = " +
                placeholder;
            params.emplace_back(std::string_view(*connectionStatus));
        }

        const auto countRows = co_await c.db().query("SELECT COUNT(*)" + where, params);
        const auto total = countRows.empty() ? std::int64_t{0}
                                             : countRows.front()[0].as<std::int64_t>().value_or(0);
        const auto rows = co_await c.db().query(
            "SELECT node.id, node.cluster_id, cluster.name, node.revision, "
            "node.node_spec_revision, node.config::text, node.runtime::text, "
            "node.registration_status, CASE WHEN node.registration_status <> 'registered' THEN "
            "'unregistered' WHEN node.status = 'enabled' AND node.last_heartbeat_at >= NOW() - "
            "INTERVAL '90 seconds' THEN 'online' ELSE 'offline' END, "
            "TO_CHAR(node.last_heartbeat_at, 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF'), "
            "node.applied_node_spec_revision, TO_CHAR(node.created_at, "
            "'YYYY-MM-DD\"T\"HH24:MI:SS.USOF'), TO_CHAR(node.updated_at, "
            "'YYYY-MM-DD\"T\"HH24:MI:SS.USOF'), node.name, node.status, "
            "node.active_release_id, node.active_manifest_digest" +
                where + " ORDER BY node.sort ASC LIMIT " + std::to_string(pageSize) + " OFFSET " +
                std::to_string(skip),
            params);

        NodePageDataDto result(c);
        result.set<"total">(total);
        result.set<"page">(page);
        result.set<"pageSize">(pageSize);
        result.set<"totalPages">(pageSize > 0 ? (total + pageSize - 1) / pageSize : 0);
        auto& items = result.ensure<"list">();
        for (const auto& row : rows) {
            fillNode(c, items.emplace_back(c), row);
        }
        co_return result;
    }

    ruvia::Task<NodeCredentialsDto> credentials(ruvia::Context& c, const std::string& tenantId,
                                                const std::string& id) const {
        const auto rows = co_await c.db().query(
            "SELECT revision, agent_id, node_secret_envelope FROM sys_node WHERE "
            "id = $1 AND tenant_id = $2 AND deleted_at IS NULL LIMIT 1",
            id, tenantId);
        if (rows.empty()) {
            service::common::throwAppError(NodeError::NOT_FOUND);
        }
        const auto envelope = rows.front()[2].value();
        if (rows.front()[1].value().value_or("").empty() || !envelope || envelope->empty()) {
            service::common::throwAppError(NodeError::CREDENTIALS_UNAVAILABLE);
        }
        service::utils::SensitiveString secret(service::utils::openSecret(*envelope));
        NodeCredentialsDto result(c);
        result.set<"nodeId">(rows.front()[1].value().value_or(""));
        result.set<"secret">(secret.view());
        result.set<"revision">(rows.front()[0].as<std::int64_t>().value_or(1));
        co_return result;
    }

    ruvia::Task<NodeLogTailDataDto>
    logs(ruvia::Context& c, const std::string& tenantId, const std::string& id, std::int64_t limit,
         const std::optional<service::log_ingest::TailCursor>& after) const {
        std::vector<ruvia::DbValue> params{ruvia::DbValue{tenantId}, ruvia::DbValue{id}};
        std::string cursorPredicate;
        if (after) {
            params.emplace_back(after->ingestedUnixMicros);
            params.emplace_back(std::string_view{after->id});
            cursorPredicate =
                " AND (entry.created_at, entry.id) > (TIMESTAMPTZ 'epoch' + $3::bigint * "
                "INTERVAL '1 microsecond', $4::uuid)";
        }
        params.emplace_back(limit);
        const auto limitParameter = "$" + std::to_string(params.size());
        const auto rows = co_await c.db().query(
            "SELECT log.id, TO_CHAR(log.occurred_at, 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF'), "
            "log.level, log.category, log.message, log.ingested_unix_micros FROM sys_node node "
            "LEFT JOIN LATERAL (SELECT entry.*, ROUND(EXTRACT(EPOCH FROM entry.created_at) * "
            "1000000)::bigint AS ingested_unix_micros FROM sys_node_log entry WHERE "
            "entry.tenant_id = node.tenant_id AND entry.node_id = node.id" +
                cursorPredicate + " ORDER BY entry.created_at DESC, entry.id DESC LIMIT " +
                limitParameter +
                ") log ON TRUE WHERE node.tenant_id = $1 AND node.id = $2 AND "
                "node.deleted_at IS NULL ORDER BY log.created_at DESC NULLS LAST, log.id DESC",
            params);
        if (rows.empty()) {
            service::common::throwAppError(NodeError::NOT_FOUND);
        }

        NodeLogTailDataDto result(c);
        auto& items = result.ensure<"list">();
        for (const auto& row : rows) {
            if (!row[0].value()) {
                continue;
            }
            auto& item = items.emplace_back(c);
            item.set<"id">(row[0].value().value_or(""));
            item.set<"occurredAt">(row[1].value().value_or(""));
            item.set<"level">(row[2].value().value_or(""));
            item.set<"category">(row[3].value().value_or(""));
            item.set<"message">(row[4].value().value_or(""));
        }
        if (!items.empty()) {
            result.set<"cursor">(service::log_ingest::encodeTailCursor(
                rows.front()[5].as<std::int64_t>().value_or(0),
                rows.front()[0].value().value_or("")));
        }
        co_return result;
    }
};

inline const NodeReadService& nodeReadService() {
    static const NodeReadService service;
    return service;
}

} // namespace service::node
