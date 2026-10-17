#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/context.h>
#include <ruvia/web/db/db.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "service/common/database.h"
#include "service/common/http.h"
#include "service/domains/node/node_error.h"
#include "service/domains/node/node_mapper.h"
#include "service/domains/node/node_types.h"
#include "service/features/live_resource/query_key.h"
#include "service/features/log_ingest/tail_cursor.h"
#include "service/features/node_presence/node_connection.h"
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
        std::optional<std::string> pattern;
        if (keyword) {
            pattern = "%" + service::common::escape_like_pattern(*keyword) + "%";
        }
        const auto filter = [&](ruvia::db_query& query) {
            source(query, tenant_id);
            if (pattern) {
                query.and_where(query.sql({"node.name ILIKE ", ""}, {query.value(*pattern)}));
            }
            if (cluster_id) {
                query.and_where(query.sql({"node.cluster_id = ", ""}, {query.value(*cluster_id)}));
            }
            if (status) {
                query.and_where(query.sql({"node.status = ", ""}, {query.value(*status)}));
            }
            if (registration_status) {
                query.and_where(query.sql({"node.registration_status = ", ""},
                    {query.value(*registration_status)}));
            }
            if (connection_status) {
                query.and_where(query.binary(connection_status_expression(query),
                    ruvia::db_binary_operator::equal, query.value(*connection_status)));
            }
        };
        const auto page_rows = co_await service::common::query_page(c, filter, project, page_size, skip);
        auto result = service::common::init_page_dto<node_page_data_dto>(
            c.pool(), page_rows.total_, page, page_size);
        auto& items = result.template ensure<"list">();
        for (const auto& row : page_rows.rows_) {
            fill_node(c, items.emplace_back(ruvia::model_options{.resource_ = c.pool()}), row);
        }
        co_return result;
    }

    // One node with exactly the list item's projection; the detail stream and
    // the list therefore always serialize the same DTO.
    ruvia::task<node_dto> get(auto& c, const std::string& tenant_id, const std::string& id) const {
        ruvia::db_query query(c.pool());
        source(query, tenant_id);
        query.and_where(query.sql({"node.id = ", ""}, {query.value(id)}));
        project(query);
        query.limit(1);
        const auto rows = co_await c.db().query(query);
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
        ruvia::db_query entries(c.pool());
        entries
            .select({entries.star("entry"),
                entries.alias(
                    entries.sql("ROUND(EXTRACT(EPOCH FROM entry.created_at) * 1000000)::bigint"),
                    "ingested_unix_micros")})
            .from("sys_node_log", "entry")
            .where(entries.sql("entry.tenant_id = node.tenant_id AND entry.node_id = node.id"));
        service::log_ingest::apply_tail_cursor_window(entries, "entry", limit, after);
        ruvia::db_query query(c.pool());
        query
            .select(service::common::sql_columns(query,
                {"log.id", "TO_CHAR(log.occurred_at, 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF')", "log.level",
                    "log.category", "log.message", "log.ingested_unix_micros"}))
            .from("sys_node", "node")
            .join(ruvia::db_join_type::left, entries, query.sql("TRUE"), "log", {.lateral_ = true})
            .where(query.sql({"node.tenant_id = ", ""}, {query.value(tenant_id)}))
            .and_where(query.sql({"node.id = ", ""}, {query.value(id)}))
            .and_where(query.sql("node.deleted_at IS NULL"))
            .order_by(query.sql("log.created_at"), ruvia::db_order_direction::desc,
                ruvia::db_nulls_order::last)
            .add_order_by(query.sql("log.id"), ruvia::db_order_direction::desc);
        const auto rows = co_await c.db().query(query);
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
    static void source(ruvia::db_query& query, const std::string& tenant_id) {
        query.from("sys_node", "node")
            .join(ruvia::db_join_type::inner, "sys_cluster",
                query.sql("cluster.tenant_id = node.tenant_id AND cluster.id = node.cluster_id"),
                "cluster")
            .where(query.sql({"node.tenant_id = ", ""}, {query.value(tenant_id)}))
            .and_where(query.sql("node.deleted_at IS NULL AND cluster.deleted_at IS NULL"));
    }

    // unregistered / online / offline, by the shared online rule of node_connection.h.
    static ruvia::db_expression connection_status_expression(ruvia::db_query& query) {
        return query.sql({"CASE WHEN node.registration_status <> 'registered' THEN 'unregistered' "
                          "WHEN ",
                             " THEN 'online' ELSE 'offline' END"},
            {node_online_predicate(query, "node")});
    }

    static void project(ruvia::db_query& query) {
        auto columns = service::common::sql_columns(query,
            {"node.id", "node.cluster_id", "cluster.name", "node.revision", "node.node_spec_revision",
                "node.config::text", "node.runtime::text", "node.registration_status"});
        columns.push_back(connection_status_expression(query));
        for (const auto column : {"TO_CHAR(node.last_heartbeat_at, 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF')",
                 "node.applied_node_spec_revision",
                 "TO_CHAR(node.created_at, 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF')",
                 "TO_CHAR(node.updated_at, 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF')", "node.name", "node.status",
                 "node.active_release_id", "node.active_manifest_digest"}) {
            columns.push_back(query.sql(column));
        }
        query.select(columns).order_by(query.sql("node.sort"), ruvia::db_order_direction::asc);
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
