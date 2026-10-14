#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/context.h>
#include <ruvia/web/db/db.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "service/common/http.h"
#include "service/domains/node/node_error.h"
#include "service/domains/node/node_mapper.h"
#include "service/domains/node/node_types.h"
#include "service/features/live_resource/query_key.h"
#include "service/features/log_ingest/tail_cursor.h"
#include "service/utils/secret.h"
#include "service/utils/sensitive_string.h"

namespace service::node {

class node_read_service final {
public:
    ruvia::task<node_page_data_dto> list(auto& c, const std::string& tenant_id,
        std::int64_t page, std::int64_t page_size, std::int64_t skip,
        const std::optional<std::string>& keyword,
        const std::optional<std::string>& cluster_id,
        const std::optional<std::string>& status,
        const std::optional<std::string>& registration_status,
        const std::optional<std::string>& connection_status) const {
        std::string where = node_source();
        std::vector<ruvia::db_value> params{ruvia::db_value{tenant_id}};
        std::optional<std::string> pattern;
        if (keyword) {
            pattern = "%" + service::common::escape_like_pattern(*keyword) + "%";
            where += " AND node.name ILIKE $" + std::to_string(params.size() + 1);
            params.emplace_back(std::string_view(*pattern));
        }
        if (cluster_id) {
            where += " AND node.cluster_id = $" + std::to_string(params.size() + 1);
            params.emplace_back(*cluster_id);
        }
        if (status) {
            where += " AND node.status = $" + std::to_string(params.size() + 1);
            params.emplace_back(std::string_view(*status));
        }
        if (registration_status) {
            where += " AND node.registration_status = $" + std::to_string(params.size() + 1);
            params.emplace_back(std::string_view(*registration_status));
        }
        if (connection_status) {
            const auto placeholder = "$" + std::to_string(params.size() + 1);
            where +=
                " AND (CASE WHEN node.registration_status <> 'registered' THEN 'unregistered' "
                "WHEN node.status = 'enabled' AND node.last_heartbeat_at >= NOW() - INTERVAL '90 "
                "seconds' THEN 'online' ELSE 'offline' END) = " +
                placeholder;
            params.emplace_back(std::string_view(*connection_status));
        }

        const auto count_rows = co_await c.db().query("SELECT COUNT(*)" + where, params);
        const auto total = count_rows.empty() ? std::int64_t{0}
                                              : count_rows.front()[0].template as<std::int64_t>().value_or(0);
        const auto rows = co_await c.db().query(
            node_columns() + where + " ORDER BY node.sort ASC LIMIT " + std::to_string(page_size) +
                " OFFSET " + std::to_string(skip),
            params);

        node_page_data_dto result(ruvia::model_options{.resource_ = c.pool()});
        result.template set<"total">(total);
        result.template set<"page">(page);
        result.template set<"page_size">(page_size);
        result.template set<"total_pages">(page_size > 0 ? (total + page_size - 1) / page_size : 0);
        auto& items = result.template ensure<"list">();
        for (const auto& row : rows) {
            fill_node(c, items.emplace_back(ruvia::model_options{.resource_ = c.pool()}), row);
        }
        co_return result;
    }

    // One node with exactly the list item's projection; the detail stream and
    // the list therefore always serialize the same DTO.
    ruvia::task<node_dto> get(auto& c, const std::string& tenant_id, const std::string& id) const {
        const auto rows = co_await c.db().query(
            node_columns() + node_source() + " AND node.id = $2 LIMIT 1", tenant_id, id);
        if (rows.empty()) {
            service::common::throw_app_error(node_error::not_found);
        }
        node_dto result(ruvia::model_options{.resource_ = c.pool()});
        fill_node(c, result, rows.front());
        co_return result;
    }

    ruvia::task<node_credentials_dto> credentials(auto& c, const std::string& tenant_id,
        const std::string& id) const {
        const auto rows = co_await c.db().query(
            "SELECT revision, agent_id, node_secret_envelope FROM sys_node WHERE "
            "id = $1 AND tenant_id = $2 AND deleted_at IS NULL LIMIT 1",
            id, tenant_id);
        if (rows.empty()) {
            service::common::throw_app_error(node_error::not_found);
        }
        const auto envelope = rows.front()[2].value();
        if (rows.front()[1].value().value_or("").empty() || !envelope || envelope->empty()) {
            service::common::throw_app_error(node_error::credentials_unavailable);
        }
        service::utils::sensitive_string secret(service::utils::open_secret(*envelope));
        node_credentials_dto result(ruvia::model_options{.resource_ = c.pool()});
        result.template set<"id">(id);
        result.template set<"node_id">(rows.front()[1].value().value_or(""));
        result.template set<"secret">(secret.view());
        result.template set<"revision">(rows.front()[0].template as<std::int64_t>().value_or(1));
        co_return result;
    }

    ruvia::task<node_log_tail_data_dto>
    logs(auto& c, const std::string& tenant_id, const std::string& id, std::int64_t limit,
        const std::optional<service::log_ingest::tail_cursor>& after) const {
        std::vector<ruvia::db_value> params{ruvia::db_value{tenant_id}, ruvia::db_value{id}};
        std::string cursor_predicate;
        if (after) {
            params.emplace_back(after->ingested_unix_micros_);
            params.emplace_back(std::string_view{after->id_});
            cursor_predicate =
                " AND (entry.created_at, entry.id) > (TIMESTAMPTZ 'epoch' + $3::bigint * "
                "INTERVAL '1 microsecond', $4::uuid)";
        }
        params.emplace_back(limit);
        const auto limit_parameter = "$" + std::to_string(params.size());
        const auto rows = co_await c.db().query(
            "SELECT log.id, TO_CHAR(log.occurred_at, 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF'), "
            "log.level, log.category, log.message, log.ingested_unix_micros FROM sys_node node "
            "LEFT JOIN LATERAL (SELECT entry.*, ROUND(EXTRACT(EPOCH FROM entry.created_at) * "
            "1000000)::bigint AS ingested_unix_micros FROM sys_node_log entry WHERE "
            "entry.tenant_id = node.tenant_id AND entry.node_id = node.id" +
                cursor_predicate + " ORDER BY entry.created_at DESC, entry.id DESC LIMIT " +
                limit_parameter +
                ") log ON TRUE WHERE node.tenant_id = $1 AND node.id = $2 AND "
                "node.deleted_at IS NULL ORDER BY log.created_at DESC NULLS LAST, log.id DESC",
            params);
        if (rows.empty()) {
            service::common::throw_app_error(node_error::not_found);
        }

        node_log_tail_data_dto result(ruvia::model_options{.resource_ = c.pool()});
        auto& items = result.template ensure<"list">();
        for (const auto& row : rows) {
            if (!row[0].value()) {
                continue;
            }
            auto& item = items.emplace_back(ruvia::model_options{.resource_ = c.pool()});
            item.template set<"id">(row[0].value().value_or(""));
            item.template set<"occurred_at">(row[1].value().value_or(""));
            item.template set<"level">(row[2].value().value_or(""));
            item.template set<"category">(row[3].value().value_or(""));
            item.template set<"message">(row[4].value().value_or(""));
        }
        if (!items.empty()) {
            result.template set<"cursor">(service::log_ingest::encode_tail_cursor(
                rows.front()[5].template as<std::int64_t>().value_or(0),
                rows.front()[0].value().value_or("")));
        }
        co_return result;
    }

private:
    // The list query and its projection predate structured queries and are
    // not rewritten in this change; the detail read reuses them verbatim.
    static std::string node_source() {
        return " FROM sys_node node INNER JOIN sys_cluster cluster ON cluster.tenant_id = "
               "node.tenant_id AND cluster.id = node.cluster_id WHERE node.tenant_id = $1 AND "
               "node.deleted_at IS NULL AND cluster.deleted_at IS NULL";
    }

    static std::string node_columns() {
        return "SELECT node.id, node.cluster_id, cluster.name, node.revision, "
               "node.node_spec_revision, node.config::text, node.runtime::text, "
               "node.registration_status, CASE WHEN node.registration_status <> 'registered' THEN "
               "'unregistered' WHEN node.status = 'enabled' AND node.last_heartbeat_at >= NOW() - "
               "INTERVAL '90 seconds' THEN 'online' ELSE 'offline' END, "
               "TO_CHAR(node.last_heartbeat_at, 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF'), "
               "node.applied_node_spec_revision, TO_CHAR(node.created_at, "
               "'YYYY-MM-DD\"T\"HH24:MI:SS.USOF'), TO_CHAR(node.updated_at, "
               "'YYYY-MM-DD\"T\"HH24:MI:SS.USOF'), node.name, node.status, "
               "node.active_release_id, node.active_manifest_digest";
    }
};

// Live-resource cache key of one node list page. The overview's node health
// part uses the same key as `/api/nodes/stream`, so both share one read.
inline std::string node_list_query_key(std::int64_t page, std::int64_t page_size,
    std::string_view keyword,
    const std::optional<std::string>& cluster_id,
    const std::optional<std::string>& status,
    const std::optional<std::string>& registration_status,
    const std::optional<std::string>& connection_status) {
    return service::live_resource::query_key("list", page, page_size, keyword, cluster_id, status,
        registration_status, connection_status);
}

inline const node_read_service& get_node_read_service() {
    static const node_read_service service;
    return service;
}

}  // namespace service::node
