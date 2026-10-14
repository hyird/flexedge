#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/context.h>
#include <ruvia/web/db/db_query.h>
#include <ruvia/web/model_object.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>

#include "service/domains/task/task_error.h"
#include "service/domains/task/task_types.h"
#include "service/features/attempt_trace/model.h"
#include "service/features/sync_runtime/attempt_record.h"

namespace service::task {

// Markers remain coalesced scheduling state. The read model groups retained
// results by version; it never changes scheduling or treats an old failed
// attempt as the state of a newer version. Node publish attempts share
// sys_sync_event but their state is the release target, so they only add
// failure counts to the node branch. Every branch is tenant-scoped.
inline const std::string task_records_sql = R"sql(
WITH history AS (
    SELECT task_id, resource_type, resource_id, version,
        (array_agg(operation ORDER BY id DESC))[1] AS operation,
        (array_agg(outcome ORDER BY id DESC))[1] AS outcome,
        (array_agg(error ORDER BY id DESC))[1] AS error,
        count(*) FILTER (WHERE outcome = 'failed') AS failures,
        max(emitted_at) AS updated_at
    FROM sys_sync_event WHERE tenant_id = $1 AND resource_type <> 'node'
    GROUP BY task_id, resource_type, resource_id, version
), node_history AS (
    SELECT task_id, resource_id, version, count(*) FILTER (WHERE outcome = 'failed') AS failures
    FROM sys_sync_event WHERE tenant_id = $1 AND resource_type = 'node'
    GROUP BY task_id, resource_id, version
), sync_records AS (
    SELECT m.id, m.resource_type, m.resource_id, m.operation, m.version,
        CASE WHEN m.is_done AND m.is_ok THEN
            CASE WHEN COALESCE(h.failures, 0) > 0 THEN 'recovered' ELSE 'completed' END
            WHEN m.is_done THEN 'failed'
            WHEN m.lease_until > NOW() THEN 'running'
            WHEN m.count_fails > 0 THEN 'retrying' ELSE 'queued' END AS status,
        m.error, GREATEST(m.count_fails, COALESCE(h.failures, 0)) AS failures,
        m.updated_at, CASE WHEN NOT m.is_done AND m.count_fails > 0
            AND m.lease_until IS NULL THEN m.next_attempt_at END AS next_attempt_at
    FROM sys_sync_task m LEFT JOIN history h ON h.task_id = m.id AND h.version = m.version
    WHERE m.tenant_id = $1
    UNION ALL
    SELECT h.task_id, h.resource_type, h.resource_id, h.operation, h.version,
        CASE WHEN h.outcome = 'completed' THEN
            CASE WHEN h.failures > 0 THEN 'recovered' ELSE 'completed' END
            ELSE 'superseded' END,
        h.error, h.failures, h.updated_at, NULL::timestamptz
    FROM history h WHERE NOT EXISTS (
        SELECT 1 FROM sys_sync_task m WHERE m.tenant_id = $1 AND m.id = h.task_id
        AND m.version = h.version)
), records AS (
    SELECT s.id, s.resource_type, s.resource_id,
        COALESCE(p.name, p.provider, z.domain, c.domain, w.config->>'name', s.resource_id::text) AS name,
        s.operation, s.version, s.status, s.error, s.failures, s.updated_at, s.next_attempt_at
    FROM sync_records s
    LEFT JOIN sys_provider p ON s.resource_type = 'provider' AND p.tenant_id = $1 AND p.id = s.resource_id
    LEFT JOIN sys_dns_zone z ON s.resource_type = 'dns_zone' AND z.tenant_id = $1 AND z.id = s.resource_id
    LEFT JOIN sys_certificate c ON s.resource_type = 'certificate' AND c.tenant_id = $1 AND c.id = s.resource_id
    LEFT JOIN sys_website w ON s.resource_type = 'website' AND w.tenant_id = $1 AND w.id = s.resource_id
    UNION ALL
    SELECT t.release_id, 'node', t.node_id, n.name, 'publish', r.generation,
        CASE WHEN t.status = 'applied' THEN
            CASE WHEN COALESCE(nh.failures, 0) > 0 THEN 'recovered' ELSE 'completed' END
            WHEN r.status = 'superseded' OR t.status = 'excluded' THEN 'superseded'
            WHEN t.status = 'failed' AND t.retryable IS TRUE THEN 'retrying'
            WHEN t.status = 'failed' THEN 'failed' ELSE 'queued' END,
        COALESCE(t.last_error, ''),
        GREATEST(CASE WHEN t.status = 'failed' THEN 1 ELSE 0 END, COALESCE(nh.failures, 0)),
        t.updated_at, NULL::timestamptz
    FROM sys_node_release_target t JOIN sys_cluster_release r
        ON r.tenant_id = t.tenant_id AND r.id = t.release_id
    JOIN sys_node n ON n.tenant_id = t.tenant_id AND n.id = t.node_id
    LEFT JOIN node_history nh ON nh.task_id = t.release_id AND nh.resource_id = t.node_id
        AND nh.version = r.generation
    WHERE t.tenant_id = $1 AND (r.status <> 'superseded' OR t.updated_at >= NOW() - INTERVAL '7 days')
), filtered AS (
    SELECT * FROM records WHERE ($2 = '' OR resource_type = $2)
        AND ($3 = '' OR status = $3)
        AND ($4 = '' OR strpos(lower(name), lower($4)) > 0 OR strpos(resource_id::text, $4) > 0)
        AND ($5::integer = 0 OR updated_at >= NOW() - $5::integer * INTERVAL '1 day')
)
)sql";

class task_service final {
public:
    ruvia::task<task_dto> detail(auto& c, const std::string& tenant, const std::string& id,
        const std::string& resource_id, std::int64_t version) {
        const auto rows = co_await c.db().query(
            task_records_sql + task_columns() +
                "FROM filtered WHERE id = $6 AND resource_id = $7 AND version = $8",
            tenant, std::string_view{}, std::string_view{}, std::string_view{}, 0, id, resource_id,
            version);
        if (rows.empty()) {
            service::common::throw_app_error(task_error::not_found);
        }
        task_dto item({.resource_ = c.pool()});
        assign(item, rows.front());
        co_return item;
    }

    ruvia::task<task_page_dto> list(auto& c, const std::string& tenant, std::int64_t page,
        std::int64_t page_size, std::int64_t skip, const std::string& type,
        const std::string& status, const std::string& keyword,
        std::int64_t days) {
        task_page_dto data({.resource_ = c.pool()});
        // One snapshot keeps the totals and rows consistent during rapid results.
        auto tx = co_await c.db().begin_transaction();
        (void)co_await tx.execute("SET TRANSACTION ISOLATION LEVEL REPEATABLE READ, READ ONLY");
        const auto counts = co_await tx.query(
            task_records_sql +
                "SELECT (SELECT count(*) FROM filtered), "
                "count(*) FILTER (WHERE status IN ('queued', 'running', 'retrying')), "
                "count(*) FILTER (WHERE status = 'failed') FROM records",
            tenant, type, status, keyword, days);
        const auto total = counts.front()[0].template as<std::int64_t>().value_or(0);
        data.template set<"total">(total);
        data.template set<"page">(page);
        data.template set<"page_size">(page_size);
        data.template set<"total_pages">((total + page_size - 1) / page_size);
        data.template set<"active">(counts.front()[1].template as<std::int64_t>().value_or(0));
        data.template set<"failed">(counts.front()[2].template as<std::int64_t>().value_or(0));
        const auto rows = co_await tx.query(task_records_sql + task_columns() +
                                                "FROM filtered ORDER BY updated_at DESC, id, "
                                                "resource_id, version DESC LIMIT $6 OFFSET $7",
            tenant, type, status, keyword, days, page_size, skip);
        auto& list = data.template ensure<"list">();
        for (const auto& row : rows) {
            auto& item = list.emplace_back(ruvia::model_options{.resource_ = c.pool()});
            assign(item, row);
        }
        co_await tx.commit();
        co_return data;
    }

    // Retained attempts of one task version, newest first, plus a summary of
    // every retained attempt. `resource_id` narrows node publish tasks, whose
    // id is the release shared by all target nodes; empty means any resource.
    ruvia::task<task_history_dto> history(auto& c, const std::string& tenant,
        const std::string& id, const std::string& resource_id, std::int64_t version) {
        task_history_dto data({.resource_ = c.pool()});
        // One snapshot keeps the summary consistent with the listed attempts.
        auto tx = co_await c.db().begin_transaction();
        (void)co_await tx.execute("SET TRANSACTION ISOLATION LEVEL REPEATABLE READ, READ ONLY");
        const auto rows = co_await tx.query(
            attempts_query(c.pool(), tenant, id, resource_id, version, history_limit + 1));
        const auto totals = co_await tx.query(summary_query(c.pool(), tenant, id, resource_id, version));
        co_await tx.commit();
        data.template set<"truncated">(rows.size() > history_limit);
        auto& list = data.template ensure<"list">();
        for (const auto& row : rows) {
            if (list.size() == history_limit) {
                break;
            }
            auto& item = list.emplace_back(ruvia::model_options{.resource_ = c.pool()});
            item.template set<"attempt">(row[0].template as<std::int64_t>().value_or(0));
            item.template set<"outcome">(row[1].value().value_or(""));
            item.template set<"error">(row[2].value().value_or(""));
            item.template set<"started_at">(row[3].value().value_or(""));
            item.template set<"emitted_at">(row[4].value().value_or(""));
            item.template set<"duration_ms">(row[5].template as<std::int64_t>().value_or(0));
            item.template set<"next_attempt_at">(row[6].value().value_or(""));
            auto detail = ruvia::json_object::parse(row[7].value().value_or(""), {.resource_ = c.pool()});
            if (!detail) {
                detail = ruvia::json_object::parse(service::sync_runtime::detail_none_json,
                    {.resource_ = c.pool()});
            }
            item.template set<"detail">(std::move(*detail));
            auto trace = ruvia::json_object::parse(row[8].value().value_or(""), {.resource_ = c.pool()});
            if (!trace) {
                trace = ruvia::json_object::parse(service::attempt_trace::summary_none_json, {.resource_ = c.pool()});
            }
            item.template set<"trace">(std::move(*trace));
        }
        task_history_summary_dto summary({.resource_ = c.pool()});
        const auto& total = totals.front();
        summary.template set<"attempt_count">(total[0].template as<std::int64_t>().value_or(0));
        summary.template set<"failure_count">(total[1].template as<std::int64_t>().value_or(0));
        summary.template set<"first_failure_at">(total[2].value().value_or(""));
        summary.template set<"last_success_at">(total[3].value().value_or(""));
        summary.template set<"next_attempt_at">(total[4].value().value_or(""));
        data.template set<"summary">(std::move(summary));
        co_return data;
    }

    // One attempt with its full trace. The attempt must belong to the task in
    // the caller's tenant; an attempt without a stored trace reads as "none".
    ruvia::task<task_attempt_trace_dto> attempt_trace(auto& c, const std::string& tenant, const std::string& id,
        std::int64_t attempt_id) {
        const auto rows = co_await c.db().query(attempt_trace_query(c.pool(), tenant, id, attempt_id));
        if (rows.empty()) {
            service::common::throw_app_error(task_error::not_found);
        }
        const auto& row = rows.front();
        task_attempt_trace_dto data({.resource_ = c.pool()});
        const bool stored = row[13].value().has_value();
        const auto resource_type = std::string(row[2].value().value_or(""));
        data.template set<"attempt_id">(row[0].value().value_or(""));
        data.template set<"task_id">(row[1].value().value_or(""));
        data.template set<"resource_type">(resource_type);
        data.template set<"resource_id">(row[3].value().value_or(""));
        data.template set<"kind">(row[4].value().value_or("none"));
        data.template set<"state">(stored ? std::string_view{"available"} : std::string_view{"none"});
        data.template set<"attempt">(row[5].template as<std::int64_t>().value_or(0));
        data.template set<"outcome">(row[6].value().value_or(""));
        data.template set<"error">(row[7].value().value_or(""));
        data.template set<"started_at">(row[8].value().value_or(""));
        data.template set<"emitted_at">(row[9].value().value_or(""));
        data.template set<"duration_ms">(row[10].template as<std::int64_t>().value_or(0));
        const auto worker_type = row[11].value();
        data.template set<"worker_type">(worker_type ? *worker_type : worker_type_for(resource_type));
        auto capture = ruvia::json_object::parse(row[12].value().value_or(""), {.resource_ = c.pool()});
        if (!capture) {
            capture = ruvia::json_object::parse(service::attempt_trace::capture_none_json, {.resource_ = c.pool()});
        }
        data.template set<"capture">(std::move(*capture));
        auto steps = ruvia::json_value::parse(row[13].value().value_or("[]"), {.resource_ = c.pool()});
        if (!steps || !steps->is_array()) {
            steps = ruvia::json_value::parse(std::string_view{"[]"}, {.resource_ = c.pool()});
        }
        data.template set<"steps">(std::move(*steps));
        co_return data;
    }

    static constexpr std::size_t history_limit{100};

    static ruvia::db_query attempts_query(std::pmr::memory_resource* resource, std::string_view tenant,
        std::string_view id, std::string_view resource_id, std::int64_t version, std::size_t limit) {
        ruvia::db_query query(resource);
        query
            .select({query.column("attempt"), query.column("outcome"), query.column("error"),
                formatted_time(query, query.column("started_at")),
                formatted_time(query, query.column("emitted_at")), query.column("duration_ms"),
                formatted_time(query, query.column("next_attempt_at")),
                query.cast(query.column("detail"), ruvia::db_data_type::text), trace_summary(query)})
            .from("sys_sync_event");
        restrict_attempts(query, tenant, id, resource_id, version);
        query.order_by(query.column("id"), ruvia::db_order_direction::desc).limit(limit);
        return query;
    }

    // Aggregates every retained attempt; next_attempt_at is the retry the
    // sync marker of this version has scheduled (node publishes have none).
    static ruvia::db_query summary_query(std::pmr::memory_resource* resource, std::string_view tenant,
        std::string_view id, std::string_view resource_id, std::int64_t version) {
        ruvia::db_query marker(resource);
        const auto marker_column = [&](std::string_view column) { return marker.column(column, "marker"); };
        marker.select(marker_column("next_attempt_at"))
            .from("sys_sync_task", "marker")
            .where(marker.binary(marker_column("tenant_id"), ruvia::db_binary_operator::equal, marker.value(tenant)))
            .and_where(marker.binary(marker_column("id"), ruvia::db_binary_operator::equal, marker.value(id)))
            .and_where(marker.binary(marker_column("version"), ruvia::db_binary_operator::equal, marker.value(version)))
            .and_where(marker.unary(ruvia::db_unary_operator::not_value, marker_column("is_done")))
            .and_where(marker.binary(marker_column("count_fails"), ruvia::db_binary_operator::greater,
                marker.value(std::int64_t{0})))
            .and_where(marker.unary(ruvia::db_unary_operator::is_null, marker_column("lease_until")));
        if (!resource_id.empty()) {
            marker.and_where(marker.binary(marker_column("resource_id"), ruvia::db_binary_operator::equal,
                marker.value(resource_id)));
        }

        ruvia::db_query query(resource);
        const auto outcome_is = [&](std::string_view outcome) {
            return query.binary(query.column("outcome"), ruvia::db_binary_operator::equal, query.value(outcome));
        };
        const auto count = query.aggregate("count", {query.star()});
        query
            .select({count, query.filter(query.aggregate("count", {query.star()}), outcome_is("failed")),
                formatted_time(query,
                    query.filter(query.aggregate("min", {query.column("emitted_at")}), outcome_is("failed"))),
                formatted_time(query,
                    query.filter(query.aggregate("max", {query.column("emitted_at")}), outcome_is("completed"))),
                formatted_time(query, query.subquery(marker))})
            .from("sys_sync_event");
        restrict_attempts(query, tenant, id, resource_id, version);
        return query;
    }

    // The attempt's full trace joined with its history row, all columns
    // qualified: both tables carry tenant_id.
    static ruvia::db_query attempt_trace_query(std::pmr::memory_resource* resource, std::string_view tenant,
        std::string_view id, std::int64_t attempt_id) {
        ruvia::db_query query(resource);
        const auto event = [&](std::string_view column) { return query.column(column, "event"); };
        const auto stored = [&](std::string_view key) {
            return query.binary(query.column("trace", "stored"), ruvia::db_binary_operator::json_get,
                query.value(key));
        };
        const auto text = [&](ruvia::db_expression value) { return query.cast(value, ruvia::db_data_type::text); };
        query
            .select({text(event("id")), text(event("task_id")), event("resource_type"), text(event("resource_id")),
                query.coalesce({query.binary(event("detail"), ruvia::db_binary_operator::json_get_text,
                                    query.value(std::string_view{"kind"})),
                    query.value(std::string_view{"none"})}),
                event("attempt"), event("outcome"), event("error"), formatted_time(query, event("started_at")),
                formatted_time(query, event("emitted_at")), event("duration_ms"),
                query.binary(query.column("trace", "stored"), ruvia::db_binary_operator::json_get_text,
                    query.value(std::string_view{"worker_type"})),
                text(stored("capture")), text(stored("steps"))})
            .from("sys_sync_event", "event")
            .join(ruvia::db_join_type::left, "sys_sync_event_trace",
                query.binary(query.column("event_id", "stored"), ruvia::db_binary_operator::equal, event("id")),
                "stored")
            .where(query.binary(event("tenant_id"), ruvia::db_binary_operator::equal, query.value(tenant)))
            .and_where(query.binary(event("task_id"), ruvia::db_binary_operator::equal, query.value(id)))
            .and_where(query.binary(event("id"), ruvia::db_binary_operator::equal, query.value(attempt_id)));
        return query;
    }

private:
    static constexpr std::string_view time_format{"YYYY-MM-DD\"T\"HH24:MI:SS.USOF"};

    // Worker type that runs attempts of a resource type; shown when no trace
    // was stored for the attempt.
    static std::string_view worker_type_for(std::string_view resource_type) noexcept {
        if (resource_type == "dns_zone") {
            return "dns-sync";
        }
        if (resource_type == "certificate") {
            return "certificate";
        }
        if (resource_type == "website") {
            return "website-dispatch";
        }
        if (resource_type == "provider") {
            return "provider-verification";
        }
        return "web";
    }

    // Stored trace summary of the history row, or the "none" summary, with
    // the attempt id the trace endpoint takes.
    static ruvia::db_expression trace_summary(ruvia::db_query& query) {
        ruvia::db_query stored(query.resource());
        stored.select(stored.column("summary", "stored"))
            .from("sys_sync_event_trace", "stored")
            .where(stored.binary(stored.column("event_id", "stored"), ruvia::db_binary_operator::equal,
                stored.column("id", "sys_sync_event")));
        const auto summary = query.coalesce({query.subquery(stored),
            query.cast(query.value(service::attempt_trace::summary_none_json), ruvia::db_data_type::jsonb)});
        const auto attempt_id = query.call("jsonb_build_object",
            {query.value(std::string_view{"attempt_id"}), query.cast(query.column("id"), ruvia::db_data_type::text)});
        return query.cast(query.binary(summary, ruvia::db_binary_operator::json_concat, attempt_id),
            ruvia::db_data_type::text);
    }

    static ruvia::db_expression formatted_time(ruvia::db_query& query, ruvia::db_expression value) {
        return query.coalesce(
            {query.call("to_char", {value, query.value(time_format)}), query.value(std::string_view{})});
    }

    static void restrict_attempts(ruvia::db_query& query, std::string_view tenant, std::string_view id,
        std::string_view resource_id, std::int64_t version) {
        const auto equals = [&](std::string_view column, ruvia::db_expression value) {
            return query.binary(query.column(column), ruvia::db_binary_operator::equal, value);
        };
        query.where(equals("tenant_id", query.value(tenant)))
            .and_where(equals("task_id", query.value(id)))
            .and_where(equals("version", query.value(version)));
        if (!resource_id.empty()) {
            query.and_where(equals("resource_id", query.value(resource_id)));
        }
    }

    static std::string task_columns() {
        return "SELECT id, resource_type, resource_id, name, operation, version, status, error, "
               "failures, "
               "TO_CHAR(updated_at, 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF'), "
               "COALESCE(TO_CHAR(next_attempt_at, 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF'), '') ";
    }
    template <typename row_type>
    static void assign(task_dto& item, const row_type& row) {
        item.template set<"id">(row[0].value().value_or(""));
        item.template set<"resource_type">(row[1].value().value_or(""));
        item.template set<"resource_id">(row[2].value().value_or(""));
        item.template set<"name">(row[3].value().value_or(""));
        item.template set<"operation">(row[4].value().value_or(""));
        item.template set<"version">(row[5].template as<std::int64_t>().value_or(0));
        item.template set<"status">(row[6].value().value_or(""));
        item.template set<"error">(row[7].value().value_or(""));
        item.template set<"failures">(row[8].template as<std::int64_t>().value_or(0));
        item.template set<"updated_at">(row[9].value().value_or(""));
        item.template set<"next_attempt_at">(row[10].value().value_or(""));
    }
};

inline task_service& get_task_service() {
    static task_service service;
    return service;
}
}  // namespace service::task
