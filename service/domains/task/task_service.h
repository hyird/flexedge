#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/context.h>
#include <ruvia/web/db/db.h>
#include <ruvia/web/model_object.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "service/domains/task/task_error.h"
#include "service/domains/task/task_types.h"
#include "service/features/attempt_trace/model.h"
#include "service/features/live_resource/fanout.h"
#include "service/features/node_presence/node_connection.h"
#include "service/features/sync_runtime/attempt_record.h"

namespace service::task {

namespace detail {

// Every task of tenant $1 ($2 the node online window in seconds,
// service::node::online_window_seconds) as CTE `tasks`: one per sync marker
// whose resource row exists, one per registered, undeleted node.
//
// Marker task: done and ok -> completed, or attention_required when the
// current version's latest successful attempt left work to do (a DNS conflict
// or unpushed records, an unverified website domain); leased -> running;
// failed before -> retrying; else queued.
// Node task: the worse of its release part (target of the newest release of
// its cluster) and its spec part (node_spec_revision not applied yet), by
// failed > retrying > queued > completed.
// `failure_count` counts the failed attempts of the current work: the
// marker's version, the node's newest release and current spec revision.
inline constexpr std::string_view tasks_sql{R"sql(
WITH markers AS (
    SELECT marker.resource_type, marker.resource_id, COALESCE(provider.kind, '') AS resource_kind,
        COALESCE(website.config->>'name', zone.domain, certificate.domain, provider.name, provider.provider, '')
            AS name,
        marker.operation, marker.version, marker.is_done AND marker.is_ok AS done,
        COALESCE(marker.lease_until > NOW(), FALSE) AS leased, marker.count_fails, marker.error,
        marker.next_attempt_at, marker.updated_at, zone.sync_status AS zone_status
    FROM sys_sync_task marker
    LEFT JOIN sys_website website ON marker.resource_type = 'website' AND website.tenant_id = marker.tenant_id
        AND website.id = marker.resource_id
    LEFT JOIN sys_dns_zone zone ON marker.resource_type = 'dns_zone' AND zone.tenant_id = marker.tenant_id
        AND zone.id = marker.resource_id
    LEFT JOIN sys_certificate certificate ON marker.resource_type = 'certificate'
        AND certificate.tenant_id = marker.tenant_id AND certificate.id = marker.resource_id
    LEFT JOIN sys_provider provider ON marker.resource_type = 'provider' AND provider.tenant_id = marker.tenant_id
        AND provider.id = marker.resource_id
    WHERE marker.tenant_id = $1 AND COALESCE(website.id, zone.id, certificate.id, provider.id) IS NOT NULL
), marker_tasks AS (
    SELECT markers.*, attempts.attempt_count, attempts.failure_count, attempts.last_at, attempts.last_success_at,
        CASE WHEN NOT markers.done THEN ''
            WHEN markers.resource_type = 'dns_zone' AND (COALESCE((success.detail->>'conflict_count')::bigint, 0) > 0
                OR markers.zone_status = 'conflict') THEN 'dns_conflict'
            WHEN markers.resource_type = 'dns_zone' AND markers.zone_status = 'failed' THEN 'dns_unpushed'
            WHEN markers.resource_type = 'website' AND EXISTS (SELECT 1 FROM jsonb_array_elements(CASE
                WHEN jsonb_typeof(success.detail->'domains') = 'array' THEN success.detail->'domains'
                ELSE '[]'::jsonb END) domain WHERE domain->>'status' <> 'verified') THEN 'domain_unverified'
            ELSE '' END AS warning
    FROM markers
    CROSS JOIN LATERAL (SELECT count(*) AS attempt_count,
            count(*) FILTER (WHERE event.outcome = 'failed' AND event.version = markers.version) AS failure_count,
            max(event.emitted_at) AS last_at,
            max(event.emitted_at) FILTER (WHERE event.outcome = 'completed') AS last_success_at
        FROM sys_sync_event event
        WHERE event.tenant_id = $1 AND event.resource_type = markers.resource_type
            AND event.resource_id = markers.resource_id) attempts
    LEFT JOIN LATERAL (SELECT event.detail FROM sys_sync_event event
        WHERE markers.done AND event.tenant_id = $1 AND event.resource_type = markers.resource_type
            AND event.resource_id = markers.resource_id AND event.version = markers.version
            AND event.outcome = 'completed'
        ORDER BY event.id DESC LIMIT 1) success ON TRUE
), marker_statused AS (
    SELECT marker_tasks.*, CASE
            WHEN marker_tasks.done THEN
                CASE WHEN marker_tasks.warning <> '' THEN 'attention_required' ELSE 'completed' END
            WHEN marker_tasks.leased THEN 'running'
            WHEN marker_tasks.count_fails > 0 THEN 'retrying'
            ELSE 'queued' END AS status
    FROM marker_tasks
), nodes AS (
    SELECT node.id, node.name, node.node_spec_revision > node.applied_node_spec_revision AS spec_pending,
        node.node_spec_revision, node.updated_at, COALESCE(node.registered_at, node.created_at) AS registered_at,
        COALESCE(node.last_heartbeat_at >= NOW() - make_interval(secs => $2), FALSE) AS heartbeat_recent,
        target.release_id, target.status AS target_status, target.retryable AS target_retryable,
        target.last_error AS target_error, target.updated_at AS target_updated
    FROM sys_node node
    LEFT JOIN LATERAL (SELECT release_target.release_id, release_target.status, release_target.retryable,
            release_target.last_error, release_target.updated_at
        FROM sys_cluster_release release
        INNER JOIN sys_node_release_target release_target ON release_target.tenant_id = release.tenant_id
            AND release_target.release_id = release.id
        WHERE release.tenant_id = $1 AND release.cluster_id = node.cluster_id AND release_target.node_id = node.id
        ORDER BY release.generation DESC LIMIT 1) target ON TRUE
    WHERE node.tenant_id = $1 AND node.registration_status = 'registered' AND node.deleted_at IS NULL
), node_tasks AS (
    SELECT nodes.*, attempts.attempt_count, attempts.failure_count, attempts.last_at, attempts.last_success_at,
        CASE nodes.target_status
            WHEN 'pending' THEN 'queued'
            WHEN 'failed' THEN CASE WHEN nodes.target_retryable IS FALSE THEN 'failed' ELSE 'retrying' END
            ELSE 'completed' END AS release_status,
        CASE WHEN NOT nodes.spec_pending THEN 'completed'
            WHEN configure.outcome = 'failed' THEN
                CASE WHEN configure.detail->'retryable' = 'false'::jsonb THEN 'failed' ELSE 'retrying' END
            ELSE 'queued' END AS spec_status,
        configure.outcome AS configure_outcome, configure.error AS configure_error
    FROM nodes
    CROSS JOIN LATERAL (SELECT count(*) AS attempt_count,
            count(*) FILTER (WHERE event.outcome = 'failed' AND ((event.operation = 'publish'
                AND event.task_id = nodes.release_id) OR (event.operation = 'configure'
                AND event.version = nodes.node_spec_revision))) AS failure_count,
            max(event.emitted_at) AS last_at,
            max(event.emitted_at) FILTER (WHERE event.outcome = 'completed') AS last_success_at
        FROM sys_sync_event event
        WHERE event.tenant_id = $1 AND event.resource_type = 'node' AND event.resource_id = nodes.id) attempts
    LEFT JOIN LATERAL (SELECT event.outcome, event.error, event.detail FROM sys_sync_event event
        WHERE event.tenant_id = $1 AND event.resource_type = 'node' AND event.resource_id = nodes.id
            AND event.operation = 'configure' AND event.version = nodes.node_spec_revision
        ORDER BY event.id DESC LIMIT 1) configure ON TRUE
), node_statused AS (
    SELECT node_tasks.*, CASE
            WHEN 'failed' IN (node_tasks.release_status, node_tasks.spec_status) THEN 'failed'
            WHEN 'retrying' IN (node_tasks.release_status, node_tasks.spec_status) THEN 'retrying'
            WHEN 'queued' IN (node_tasks.release_status, node_tasks.spec_status) THEN 'queued'
            ELSE 'completed' END AS status
    FROM node_tasks
), tasks AS (
    SELECT marker_statused.resource_type, marker_statused.resource_id, marker_statused.resource_kind,
        marker_statused.name, marker_statused.operation, marker_statused.status,
        CASE WHEN marker_statused.status = 'attention_required' THEN marker_statused.warning ELSE '' END
            AS warning,
        CASE WHEN marker_statused.status = 'retrying' THEN marker_statused.error ELSE '' END AS error,
        CASE WHEN marker_statused.status = 'retrying' THEN marker_statused.next_attempt_at END AS next_attempt_at,
        GREATEST(marker_statused.updated_at, marker_statused.last_at) AS updated_at,
        marker_statused.last_success_at, marker_statused.attempt_count, marker_statused.failure_count,
        FALSE AS node_offline
    FROM marker_statused
    UNION ALL
    SELECT 'node', node_statused.id, '', node_statused.name,
        CASE WHEN node_statused.spec_pending THEN 'configure' ELSE 'publish' END, node_statused.status, '',
        CASE WHEN node_statused.status NOT IN ('failed', 'retrying') THEN ''
            WHEN node_statused.status = node_statused.release_status THEN COALESCE(node_statused.target_error, '')
            ELSE COALESCE(node_statused.configure_error, '') END,
        NULL::timestamptz,
        GREATEST(node_statused.target_updated, node_statused.last_at,
            CASE WHEN node_statused.spec_pending AND node_statused.configure_outcome IS NULL
                THEN node_statused.updated_at END,
            node_statused.registered_at),
        node_statused.last_success_at, node_statused.attempt_count, node_statused.failure_count,
        NOT node_statused.heartbeat_recent AND node_statused.status <> 'completed'
    FROM node_statused
)
)sql"};

inline constexpr std::string_view task_columns_sql{R"sql(
SELECT resource_type, resource_id::text, resource_kind, name, operation, status, warning, error,
    COALESCE(TO_CHAR(next_attempt_at, 'YYYY-MM-DD"T"HH24:MI:SS.USOF'), ''),
    COALESCE(TO_CHAR(updated_at, 'YYYY-MM-DD"T"HH24:MI:SS.USOF'), ''),
    COALESCE(TO_CHAR(last_success_at, 'YYYY-MM-DD"T"HH24:MI:SS.USOF'), ''),
    attempt_count, failure_count, node_offline
FROM tasks
)sql"};

// Filters of the list: $3 status or group ("active", "attention"), $4
// resource type, $5 keyword (name or resource id, case-insensitive).
inline constexpr std::string_view list_filter_sql{R"sql(
WHERE ($3 = '' OR status = $3 OR ($3 = 'active' AND status IN ('queued', 'running', 'retrying'))
        OR ($3 = 'attention' AND status IN ('retrying', 'failed', 'attention_required')))
    AND ($4 = '' OR resource_type = $4)
    AND ($5 = '' OR strpos(lower(name), lower($5)) > 0 OR strpos(resource_id::text, lower($5)) > 0)
)sql"};

inline constexpr std::string_view list_counts_sql{R"sql(
SELECT (SELECT count(*) FROM tasks LIST_FILTER), count(*) FILTER (WHERE status IN ('queued', 'running', 'retrying')),
    count(*) FILTER (WHERE status = 'failed')
FROM tasks
)sql"};

inline constexpr std::string_view list_order_sql{R"sql(
ORDER BY updated_at DESC, resource_type, resource_id LIMIT $6 OFFSET $7
)sql"};

inline constexpr std::string_view detail_filter_sql{R"sql(
WHERE resource_type = $3 AND resource_id = $4::uuid
)sql"};

// Attempts of task ($2 resource type, $3 resource id) in tenant $1 with the
// stored trace summary ($4 the "none" summary) and its attempt id.
inline constexpr std::string_view attempts_sql{R"sql(
SELECT event.id::text, event.attempt, event.operation, event.outcome, event.error,
    COALESCE(TO_CHAR(event.started_at, 'YYYY-MM-DD"T"HH24:MI:SS.USOF'), ''),
    COALESCE(TO_CHAR(event.emitted_at, 'YYYY-MM-DD"T"HH24:MI:SS.USOF'), ''), event.duration_ms,
    COALESCE(TO_CHAR(event.next_attempt_at, 'YYYY-MM-DD"T"HH24:MI:SS.USOF'), ''), event.detail::text,
    (COALESCE(stored.summary, $4::jsonb) || jsonb_build_object('attempt_id', event.id::text))::text
FROM sys_sync_event event
LEFT JOIN sys_sync_event_trace stored ON stored.event_id = event.id
WHERE event.tenant_id = $1 AND event.resource_type = $2 AND event.resource_id = $3::uuid
)sql"};

// Newest first; $5 the limit.
inline constexpr std::string_view history_order_sql{R"sql(
ORDER BY event.id DESC LIMIT $5
)sql"};

// The attempts of the comma-separated ids $5, oldest first.
inline constexpr std::string_view recorded_filter_sql{R"sql(
AND event.id = ANY(string_to_array($5, ',')::bigint[]) ORDER BY event.id
)sql"};

// Aggregates only retained attempts. Live scheduling belongs to the task
// snapshot, not this history summary.
inline constexpr std::string_view summary_sql{R"sql(
SELECT count(*), count(*) FILTER (WHERE event.outcome = 'failed'),
    COALESCE(TO_CHAR(min(event.emitted_at) FILTER (WHERE event.outcome = 'failed'),
        'YYYY-MM-DD"T"HH24:MI:SS.USOF'), ''),
    COALESCE(TO_CHAR(max(event.emitted_at) FILTER (WHERE event.outcome = 'completed'),
        'YYYY-MM-DD"T"HH24:MI:SS.USOF'), '')
FROM sys_sync_event event
WHERE event.tenant_id = $1 AND event.resource_type = $2 AND event.resource_id = $3::uuid
)sql"};

// Attempt $4 of task ($2, $3) in tenant $1 with its stored trace, if any.
inline constexpr std::string_view trace_sql{R"sql(
SELECT event.id::text, event.resource_type, event.resource_id::text, COALESCE(event.detail->>'kind', 'none'),
    event.attempt, event.outcome, event.error,
    COALESCE(TO_CHAR(event.started_at, 'YYYY-MM-DD"T"HH24:MI:SS.USOF'), ''),
    COALESCE(TO_CHAR(event.emitted_at, 'YYYY-MM-DD"T"HH24:MI:SS.USOF'), ''), event.duration_ms,
    stored.trace->>'worker_type', (stored.trace->'capture')::text, (stored.trace->'steps')::text
FROM sys_sync_event event
LEFT JOIN sys_sync_event_trace stored ON stored.event_id = event.id
WHERE event.tenant_id = $1 AND event.resource_type = $2 AND event.resource_id = $3::uuid AND event.id = $4
)sql"};

inline std::string statement(std::initializer_list<std::string_view> parts) {
    std::string text;
    for (const auto part : parts) {
        text += part;
    }
    return text;
}

inline const std::string& list_counts_statement() {
    static const std::string sql = [] {
        auto counts = std::string(list_counts_sql);
        constexpr std::string_view placeholder{"LIST_FILTER"};
        counts.replace(counts.find(placeholder), placeholder.size(), list_filter_sql);
        return statement({tasks_sql, counts});
    }();
    return sql;
}

inline const std::string& list_statement() {
    static const std::string sql = statement({tasks_sql, task_columns_sql, list_filter_sql, list_order_sql});
    return sql;
}

inline const std::string& detail_statement() {
    static const std::string sql = statement({tasks_sql, task_columns_sql, detail_filter_sql});
    return sql;
}

inline const std::string& history_statement() {
    static const std::string sql = statement({attempts_sql, history_order_sql});
    return sql;
}

inline const std::string& recorded_statement() {
    static const std::string sql = statement({attempts_sql, recorded_filter_sql});
    return sql;
}

}  // namespace detail

struct list_filter final {
    std::string status_;
    std::string type_;
    std::string keyword_;
};

class task_service final {
public:
    static constexpr std::int64_t history_limit{50};

    ruvia::task<task_page_dto> list(auto& c, const std::string& tenant, std::int64_t page, std::int64_t page_size,
        std::int64_t skip, const list_filter& filter) {
        task_page_dto data({.resource_ = c.pool()});
        // One snapshot keeps the totals and rows consistent during rapid results.
        auto tx = co_await c.db().begin_transaction();
        (void)co_await tx.execute("SET TRANSACTION ISOLATION LEVEL REPEATABLE READ, READ ONLY");
        const auto counts = co_await tx.query(detail::list_counts_statement(), tenant,
            service::node::online_window_seconds, filter.status_, filter.type_, filter.keyword_);
        const auto rows = co_await tx.query(detail::list_statement(), tenant, service::node::online_window_seconds,
            filter.status_, filter.type_, filter.keyword_, page_size, skip);
        co_await tx.commit();
        const auto total = counts.front()[0].template as<std::int64_t>().value_or(0);
        data.template set<"total">(total);
        data.template set<"page">(page);
        data.template set<"page_size">(page_size);
        data.template set<"total_pages">((total + page_size - 1) / page_size);
        data.template set<"active">(counts.front()[1].template as<std::int64_t>().value_or(0));
        data.template set<"failed">(counts.front()[2].template as<std::int64_t>().value_or(0));
        auto& list = data.template ensure<"list">();
        for (const auto& row : rows) {
            assign_task(list.emplace_back(ruvia::model_options{.resource_ = c.pool()}), row);
        }
        co_return data;
    }

    ruvia::task<task_dto> detail(auto& c, const std::string& tenant, const std::string& type, const std::string& id) {
        const auto rows = co_await c.db().query(detail::detail_statement(), tenant,
            service::node::online_window_seconds, type, id);
        if (rows.empty()) {
            service::common::throw_app_error(task_error::not_found);
        }
        task_dto data({.resource_ = c.pool()});
        assign_task(data, rows.front());
        co_return data;
    }

    // The newest attempts of the task, plus a summary of every retained attempt.
    ruvia::task<task_history_dto> history(auto& c, const std::string& tenant, const std::string& type,
        const std::string& id) {
        task_history_dto data({.resource_ = c.pool()});
        // One snapshot keeps the summary consistent with the listed attempts.
        auto tx = co_await c.db().begin_transaction();
        (void)co_await tx.execute("SET TRANSACTION ISOLATION LEVEL REPEATABLE READ, READ ONLY");
        const auto rows = co_await tx.query(detail::history_statement(), tenant, type, id,
            service::attempt_trace::summary_none_json, history_limit);
        const auto totals = co_await tx.query(detail::summary_sql, tenant, type, id);
        co_await tx.commit();
        auto& list = data.template ensure<"list">();
        for (const auto& row : rows) {
            assign_attempt(list.emplace_back(ruvia::model_options{.resource_ = c.pool()}), row, c.pool());
        }
        data.template set<"summary">(read_summary(totals, c.pool()));
        co_return data;
    }

    // `attempt` events (task_attempt_event_dto JSON) of the attempts these
    // notices recorded for the task, oldest first so a client prepending each
    // one keeps newest-first order; attempts already removed by retention are
    // skipped.
    ruvia::task<std::vector<std::string>> recorded_attempts(auto& read, const std::string& tenant,
        const std::string& type, const std::string& id,
        std::span<const service::live_resource::task_attempt_notice> notices) {
        std::vector<std::int64_t> event_ids;
        for (const auto& notice : notices) {
            if (std::find(event_ids.begin(), event_ids.end(), notice.event_id_) == event_ids.end()) {
                event_ids.push_back(notice.event_id_);
            }
        }
        std::vector<std::string> events;
        if (event_ids.empty()) {
            co_return events;
        }
        std::string ids;
        for (const auto event_id : event_ids) {
            if (!ids.empty()) {
                ids += ',';
            }
            ids += std::to_string(event_id);
        }
        // The summary is read in the same snapshot as the attempts.
        auto tx = co_await read.db().begin_transaction();
        (void)co_await tx.execute("SET TRANSACTION ISOLATION LEVEL REPEATABLE READ, READ ONLY");
        const auto rows = co_await tx.query(detail::recorded_statement(), tenant, type, id,
            service::attempt_trace::summary_none_json, std::string_view(ids));
        const auto totals = co_await tx.query(detail::summary_sql, tenant, type, id);
        co_await tx.commit();
        for (const auto& row : rows) {
            task_attempt_event_dto event({.resource_ = read.pool()});
            attempt_dto item({.resource_ = read.pool()});
            assign_attempt(item, row, read.pool());
            event.template set<"item">(std::move(item));
            event.template set<"summary">(read_summary(totals, read.pool()));
            events.emplace_back(ruvia::to_json(event, {.resource_ = read.pool()}));
        }
        co_return events;
    }

    // One attempt of the task with its full trace; an attempt without a
    // stored trace reads as "none".
    ruvia::task<attempt_trace_dto> attempt_trace(auto& c, const std::string& tenant, const std::string& type,
        const std::string& id, std::int64_t attempt_id) {
        const auto rows = co_await c.db().query(detail::trace_sql, tenant, type, id, attempt_id);
        if (rows.empty()) {
            service::common::throw_app_error(task_error::attempt_not_found);
        }
        const auto& row = rows.front();
        attempt_trace_dto data({.resource_ = c.pool()});
        const bool stored = row[12].value().has_value();
        const auto resource_type = std::string(row[1].value().value_or(""));
        data.template set<"attempt_id">(row[0].value().value_or(""));
        data.template set<"resource_type">(resource_type);
        data.template set<"resource_id">(row[2].value().value_or(""));
        data.template set<"kind">(row[3].value().value_or("none"));
        data.template set<"state">(stored ? std::string_view{"available"} : std::string_view{"none"});
        data.template set<"attempt">(row[4].template as<std::int64_t>().value_or(0));
        data.template set<"outcome">(row[5].value().value_or(""));
        data.template set<"error">(row[6].value().value_or(""));
        data.template set<"started_at">(row[7].value().value_or(""));
        data.template set<"emitted_at">(row[8].value().value_or(""));
        data.template set<"duration_ms">(row[9].template as<std::int64_t>().value_or(0));
        const auto worker_type = row[10].value();
        data.template set<"worker_type">(worker_type ? *worker_type : worker_type_for(resource_type));
        auto capture = ruvia::json_object::parse(row[11].value().value_or(""), {.resource_ = c.pool()});
        if (!capture) {
            capture = ruvia::json_object::parse(service::attempt_trace::capture_none_json, {.resource_ = c.pool()});
        }
        data.template set<"capture">(std::move(*capture));
        auto steps = ruvia::json_value::parse(row[12].value().value_or("[]"), {.resource_ = c.pool()});
        if (!steps || !steps->is_array()) {
            steps = ruvia::json_value::parse(std::string_view{"[]"}, {.resource_ = c.pool()});
        }
        data.template set<"steps">(std::move(*steps));
        co_return data;
    }

private:
    template <typename row_type>
    static void assign_task(task_dto& item, const row_type& row) {
        item.template set<"resource_type">(row[0].value().value_or(""));
        item.template set<"resource_id">(row[1].value().value_or(""));
        item.template set<"resource_kind">(row[2].value().value_or(""));
        item.template set<"name">(row[3].value().value_or(""));
        item.template set<"operation">(row[4].value().value_or(""));
        item.template set<"status">(row[5].value().value_or(""));
        item.template set<"warning">(row[6].value().value_or(""));
        item.template set<"error">(row[7].value().value_or(""));
        item.template set<"next_attempt_at">(row[8].value().value_or(""));
        item.template set<"updated_at">(row[9].value().value_or(""));
        item.template set<"last_success_at">(row[10].value().value_or(""));
        item.template set<"attempt_count">(row[11].template as<std::int64_t>().value_or(0));
        item.template set<"failure_count">(row[12].template as<std::int64_t>().value_or(0));
        item.template set<"node_offline">(row[13].template as<bool>().value_or(false));
    }

    template <typename row_type>
    static void assign_attempt(attempt_dto& item, const row_type& row, std::pmr::memory_resource* pool) {
        item.template set<"id">(row[0].value().value_or(""));
        item.template set<"attempt">(row[1].template as<std::int64_t>().value_or(0));
        item.template set<"operation">(row[2].value().value_or(""));
        item.template set<"outcome">(row[3].value().value_or(""));
        item.template set<"error">(row[4].value().value_or(""));
        item.template set<"started_at">(row[5].value().value_or(""));
        item.template set<"emitted_at">(row[6].value().value_or(""));
        item.template set<"duration_ms">(row[7].template as<std::int64_t>().value_or(0));
        item.template set<"next_attempt_at">(row[8].value().value_or(""));
        auto detail = ruvia::json_object::parse(row[9].value().value_or(""), {.resource_ = pool});
        if (!detail) {
            detail = ruvia::json_object::parse(service::sync_runtime::detail_none_json, {.resource_ = pool});
        }
        item.template set<"detail">(std::move(*detail));
        auto trace = ruvia::json_object::parse(row[10].value().value_or(""), {.resource_ = pool});
        if (!trace) {
            trace = ruvia::json_object::parse(service::attempt_trace::summary_none_json, {.resource_ = pool});
        }
        item.template set<"trace">(std::move(*trace));
    }

    static history_summary_dto read_summary(const ruvia::db_rows& totals, std::pmr::memory_resource* pool) {
        history_summary_dto summary({.resource_ = pool});
        const auto& total = totals.front();
        summary.template set<"attempt_count">(total[0].template as<std::int64_t>().value_or(0));
        summary.template set<"failure_count">(total[1].template as<std::int64_t>().value_or(0));
        summary.template set<"first_failure_at">(total[2].value().value_or(""));
        summary.template set<"last_success_at">(total[3].value().value_or(""));
        return summary;
    }

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
};

inline task_service& get_task_service() {
    static task_service service;
    return service;
}

}  // namespace service::task
