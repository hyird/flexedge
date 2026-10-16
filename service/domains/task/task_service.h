#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/context.h>
#include <ruvia/web/db/db.h>
#include <ruvia/web/db/db_query.h>
#include <ruvia/web/model_object.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "service/domains/node/node_connection.h"
#include "service/domains/task/task_error.h"
#include "service/domains/task/task_types.h"
#include "service/features/attempt_trace/model.h"
#include "service/features/live_resource/fanout.h"
#include "service/features/sync_runtime/attempt_record.h"

namespace service::task {

namespace detail {

inline constexpr std::string_view time_format{"YYYY-MM-DD\"T\"HH24:MI:SS.USOF"};

inline std::string formatted(std::string_view expression) {
    return "COALESCE(TO_CHAR(" + std::string(expression) + ", 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF'), '')";
}

// TRUE when the resource `type`/`id` (SQL expressions) no longer exists
// undeleted in tenant $1.
inline std::string resource_deleted_sql(std::string_view type, std::string_view id) {
    const auto live = [&](std::string_view table) {
        return "NOT EXISTS (SELECT 1 FROM " + std::string(table) + " live WHERE live.tenant_id = $1 AND live.id = " +
               std::string(id) + " AND live.deleted_at IS NULL)";
    };
    return "CASE " + std::string(type) + " WHEN 'website' THEN " + live("sys_website") + " WHEN 'cluster' THEN " +
           live("sys_cluster") + " WHEN 'node' THEN " + live("sys_node") + " WHEN 'dns_zone' THEN " +
           live("sys_dns_zone") + " WHEN 'certificate' THEN " + live("sys_certificate") + " WHEN 'provider' THEN " +
           live("sys_provider") + " ELSE FALSE END";
}

// Every subtask of tenant $1 with its effective status. A settled subtask
// keeps its stored state; an open one follows the work doing it: the marker
// it owns (sync work), its release target (node publish) or its node
// (node configuration). `warning`: a completed subtask that left work to do
// (a DNS conflict or unpushed records, an unverified website domain).
inline const std::string& subtask_sql() {
    static const std::string sql =
        "events AS (SELECT event.subtask_id, count(*) AS attempts, count(*) FILTER (WHERE event.outcome = "
        "'failed') AS failures, max(event.emitted_at) AS last_at, (array_agg(event.outcome ORDER BY event.id "
        "DESC))[1] AS last_outcome, (array_agg(event.error ORDER BY event.id DESC))[1] AS last_error, "
        "(array_agg(event.detail ORDER BY event.id DESC))[1] AS last_detail FROM sys_sync_event event WHERE "
        "event.tenant_id = $1 AND event.subtask_id IS NOT NULL GROUP BY event.subtask_id), "
        "base AS (SELECT subtask.*, marker.id AS marker_id, COALESCE(marker.is_done AND marker.is_ok, FALSE) "
        "AS marker_ok, COALESCE(marker.lease_until > NOW(), FALSE) AS marker_running, marker.count_fails AS "
        "marker_fails, marker.next_attempt_at AS marker_next, marker.error AS marker_error, marker.updated_at "
        "AS marker_updated, target.status AS target_status, target.retryable AS target_retryable, "
        "target.last_error AS target_error, target.applied_at AS target_applied, target.updated_at AS "
        "target_updated, node.id AS node_id, node.deleted_at AS node_deleted, COALESCE(" +
        service::node::node_heartbeat_recent_sql() +
        ", FALSE) AS node_online, zone.sync_status AS zone_status, COALESCE(events.attempts, 0) AS attempts, "
        "COALESCE(events.failures, 0) AS failures, events.last_at, events.last_outcome, events.last_error, "
        "events.last_detail FROM sys_operation_subtask subtask LEFT JOIN sys_sync_task marker ON "
        "marker.subtask_id = subtask.id LEFT JOIN LATERAL (SELECT release_target.status, "
        "release_target.retryable, release_target.last_error, release_target.applied_at, "
        "release_target.updated_at FROM sys_node_release_target release_target INNER JOIN sys_cluster_release "
        "release ON release.tenant_id = release_target.tenant_id AND release.id = release_target.release_id "
        "WHERE release_target.subtask_id = subtask.id ORDER BY release.generation DESC LIMIT 1) target ON TRUE "
        "LEFT JOIN sys_node node ON subtask.resource_type = 'node' AND node.tenant_id = subtask.tenant_id AND "
        "node.id = subtask.resource_id LEFT JOIN sys_dns_zone zone ON subtask.resource_type = 'dns_zone' AND "
        "zone.tenant_id = subtask.tenant_id AND zone.id = subtask.resource_id LEFT JOIN events ON "
        "events.subtask_id = subtask.id WHERE subtask.tenant_id = $1), "
        "warned AS (SELECT base.*, CASE WHEN base.kind LIKE 'dns%' AND "
        "COALESCE((base.last_detail->>'conflict_count')::bigint, 0) > 0 THEN 'dns_conflict' WHEN base.kind "
        "LIKE 'dns%' AND base.marker_id IS NOT NULL AND base.zone_status = 'conflict' THEN 'dns_conflict' WHEN "
        "base.kind LIKE 'dns%' AND base.marker_id IS NOT NULL AND base.zone_status = 'failed' THEN "
        "'dns_unpushed' WHEN base.kind = 'website_check' AND EXISTS (SELECT 1 FROM jsonb_array_elements(CASE "
        "WHEN jsonb_typeof(base.last_detail->'domains') = 'array' THEN base.last_detail->'domains' ELSE "
        "'[]'::jsonb END) domain WHERE domain->>'status' <> 'verified') THEN 'domain_unverified' ELSE '' END "
        "AS warning_candidate FROM base), "
        "statused AS (SELECT warned.*, CASE WHEN warned.state = 'merged' THEN 'merged' WHEN warned.state = "
        "'cancelled' THEN 'cancelled' WHEN warned.state = 'completed' THEN CASE WHEN "
        "warned.warning_candidate <> '' THEN 'attention_required' ELSE 'completed' END WHEN warned.kind = "
        "'node_publish' THEN CASE warned.target_status WHEN 'applied' THEN 'completed' WHEN 'excluded' THEN "
        "'cancelled' WHEN 'pending' THEN 'queued' WHEN 'failed' THEN CASE WHEN warned.target_retryable IS "
        "FALSE THEN 'failed' ELSE 'retrying' END ELSE 'merged' END WHEN warned.kind = 'node_config' THEN CASE "
        "WHEN warned.node_id IS NULL OR warned.node_deleted IS NOT NULL THEN 'cancelled' WHEN "
        "warned.last_outcome = 'failed' THEN CASE WHEN COALESCE((warned.last_detail->>'retryable')::boolean, "
        "TRUE) THEN 'retrying' ELSE 'failed' END ELSE 'queued' END WHEN warned.marker_id IS NULL THEN 'merged' "
        "WHEN warned.marker_ok THEN CASE WHEN warned.warning_candidate <> '' THEN 'attention_required' ELSE "
        "'completed' END WHEN warned.marker_running THEN 'running' WHEN warned.marker_fails > 0 THEN "
        "'retrying' ELSE 'queued' END AS status FROM warned), "
        "shaped AS (SELECT statused.*, CASE WHEN statused.status = 'attention_required' THEN "
        "statused.warning_candidate ELSE '' END AS warning, CASE WHEN statused.status IN ('completed', "
        "'attention_required', 'failed', 'merged', 'cancelled') THEN COALESCE(statused.closed_at, CASE WHEN "
        "statused.kind = 'node_publish' THEN COALESCE(statused.target_applied, statused.target_updated) END, "
        "statused.last_at, statused.marker_updated, statused.created_at) END AS finished, CASE WHEN "
        "statused.status = 'retrying' AND statused.kind NOT IN ('node_publish', 'node_config') THEN "
        "statused.marker_next END AS next_at, CASE WHEN statused.status IN ('retrying', 'failed') THEN "
        "COALESCE(NULLIF(CASE WHEN statused.kind = 'node_publish' THEN statused.target_error WHEN statused.kind "
        "= 'node_config' THEN statused.last_error ELSE statused.marker_error END, ''), statused.last_error, '') "
        "ELSE '' END AS error_text, GREATEST(statused.created_at, statused.closed_at, statused.last_at, "
        "CASE WHEN statused.marker_id IS NOT NULL THEN statused.marker_updated END, statused.target_updated) "
        "AS activity FROM statused)";
    return sql;
}

// Tasks of tenant $1 with their rolled-up status:
// retrying > running > queued > partial_failed / failed > attention_required >
// completed > merged / cancelled.
inline const std::string& operation_sql() {
    static const std::string sql =
        "WITH " + subtask_sql() +
        ", rolled AS (SELECT operation.id, count(shaped.id) AS subtask_count, count(shaped.id) FILTER (WHERE "
        "shaped.status IN ('completed', 'attention_required', 'failed', 'merged', 'cancelled')) AS "
        "settled_count, count(shaped.id) FILTER (WHERE shaped.status IN ('retrying', 'failed')) AS "
        "failed_count, CASE WHEN count(shaped.id) = 0 THEN 'completed' WHEN bool_or(shaped.status = "
        "'retrying') THEN 'retrying' WHEN bool_or(shaped.status = 'running') THEN 'running' WHEN "
        "bool_or(shaped.status = 'queued') THEN 'queued' WHEN bool_or(shaped.status = 'failed') AND "
        "bool_or(shaped.status IN ('completed', 'attention_required')) THEN 'partial_failed' WHEN "
        "bool_or(shaped.status = 'failed') THEN 'failed' WHEN bool_or(shaped.status = 'attention_required') "
        "THEN 'attention_required' WHEN bool_or(shaped.status = 'completed') THEN 'completed' WHEN "
        "bool_and(shaped.status = 'merged') THEN 'merged' ELSE 'cancelled' END AS status, min(shaped.next_at) "
        "AS next_at, GREATEST(operation.created_at, max(shaped.activity)) AS updated, max(shaped.finished) AS "
        "finished, (array_agg(shaped.error_text ORDER BY shaped.created_at, shaped.id) FILTER (WHERE "
        "shaped.status IN ('retrying', 'failed')))[1] AS error_text FROM sys_operation operation LEFT JOIN "
        "shaped ON shaped.operation_id = operation.id WHERE operation.tenant_id = $1 GROUP BY operation.id), "
        "operations AS (SELECT operation.id, operation.action, operation.target_type, operation.target_id, "
        "operation.target_name, " +
        resource_deleted_sql("operation.target_type", "operation.target_id") +
        " AS target_deleted, operation.actor_type, operation.actor_id, operation.actor_name, operation.created_at, "
        "rolled.updated, CASE WHEN rolled.settled_count = rolled.subtask_count THEN COALESCE(rolled.finished, "
        "operation.created_at) END AS finished, rolled.status, rolled.subtask_count, rolled.settled_count, "
        "rolled.failed_count, rolled.next_at, COALESCE(rolled.error_text, '') AS error_text, "
        "operation.summary, CASE WHEN operation.target_type = 'provider' THEN (SELECT provider.kind FROM "
        "sys_provider provider WHERE provider.tenant_id = $1 AND provider.id = operation.target_id) END AS "
        "target_kind FROM sys_operation operation INNER JOIN rolled ON rolled.id = operation.id) ";
    return sql;
}

inline std::string operation_columns() {
    return "SELECT id::text, action, target_type, target_id::text, target_name, target_deleted, actor_type, "
           "actor_name, " +
           formatted("created_at") + ", " + formatted("updated") + ", " + formatted("finished") +
           ", status, subtask_count, settled_count, failed_count, " + formatted("next_at") +
           ", error_text, summary::text, COALESCE(target_kind, '') ";
}

// Filters of the list (2.1 of the contract): $2 status or group, $3 action,
// $4 target type, $5 actor ("me" with $6 the session's administrator,
// "system"), $7 days, $8 keyword.
inline constexpr std::string_view list_filter_sql{
    "WHERE ($2 = '' OR status = $2 OR ($2 = 'active' AND status IN ('queued', 'running', 'retrying')) OR ($2 "
    "= 'attention' AND status IN ('retrying', 'failed', 'partial_failed', 'attention_required'))) AND ($3 = '' "
    "OR action = $3) AND ($4 = '' OR target_type = $4) AND ($5 = '' OR ($5 = 'me' AND actor_id::text = $6) OR "
    "($5 = 'system' AND actor_type = 'system')) AND ($7::integer = 0 OR created_at >= NOW() - $7::integer * "
    "INTERVAL '1 day') AND ($8 = '' OR strpos(lower(target_name), lower($8)) > 0 OR strpos(target_id::text, "
    "lower($8)) > 0) "};

}  // namespace detail

struct list_filter final {
    std::string status_;
    std::string action_;
    std::string target_type_;
    std::string actor_;
    std::string admin_id_;
    std::int64_t days_{};
    std::string keyword_;
};

class task_service final {
public:
    ruvia::task<operation_page_dto> list(auto& c, const std::string& tenant, std::int64_t page,
        std::int64_t page_size, std::int64_t skip, const list_filter& filter) {
        operation_page_dto data({.resource_ = c.pool()});
        // One snapshot keeps the totals and rows consistent during rapid results.
        auto tx = co_await c.db().begin_transaction();
        (void)co_await tx.execute("SET TRANSACTION ISOLATION LEVEL REPEATABLE READ, READ ONLY");
        const auto counts = co_await tx.query(detail::operation_sql() +
                                                  "SELECT (SELECT count(*) FROM operations " +
                                                  std::string(detail::list_filter_sql) +
                                                  "), count(*) FILTER (WHERE status IN ('queued', 'running', "
                                                  "'retrying')), count(*) FILTER (WHERE status IN ('retrying', "
                                                  "'failed', 'partial_failed', 'attention_required')) FROM operations",
            tenant, filter.status_, filter.action_, filter.target_type_, filter.actor_, filter.admin_id_, filter.days_,
            filter.keyword_);
        const auto total = counts.front()[0].template as<std::int64_t>().value_or(0);
        data.template set<"total">(total);
        data.template set<"page">(page);
        data.template set<"page_size">(page_size);
        data.template set<"total_pages">((total + page_size - 1) / page_size);
        data.template set<"active">(counts.front()[1].template as<std::int64_t>().value_or(0));
        data.template set<"attention">(counts.front()[2].template as<std::int64_t>().value_or(0));
        const auto rows = co_await tx.query(detail::operation_sql() + detail::operation_columns() +
                                                "FROM operations " + std::string(detail::list_filter_sql) +
                                                "ORDER BY created_at DESC, id DESC LIMIT $9 OFFSET $10",
            tenant, filter.status_, filter.action_, filter.target_type_, filter.actor_, filter.admin_id_, filter.days_,
            filter.keyword_, page_size, skip);
        co_await tx.commit();
        auto& list = data.template ensure<"list">();
        for (const auto& row : rows) {
            assign_operation(list.emplace_back(ruvia::model_options{.resource_ = c.pool()}), row, c.pool());
        }
        co_return data;
    }

    // One task with its subtasks. `subtask_id`, when given, must belong to it.
    ruvia::task<operation_detail_dto> detail(auto& c, const std::string& tenant, const std::string& id,
        const std::string& subtask_id = {}) {
        auto tx = co_await c.db().begin_transaction();
        (void)co_await tx.execute("SET TRANSACTION ISOLATION LEVEL REPEATABLE READ, READ ONLY");
        const auto rows = co_await tx.query(
            detail::operation_sql() + detail::operation_columns() + "FROM operations WHERE id = $2::uuid", tenant, id);
        if (rows.empty()) {
            co_await tx.commit();
            service::common::throw_app_error(task_error::not_found);
        }
        const auto subtasks = co_await tx.query(
            "WITH " + detail::subtask_sql() +
                " SELECT shaped.id::text, COALESCE(shaped.parent_id::text, ''), shaped.kind, shaped.resource_type, "
                "shaped.resource_id::text, shaped.resource_name, " +
                detail::resource_deleted_sql("shaped.resource_type", "shaped.resource_id") +
                ", shaped.status, shaped.warning, shaped.kind IN ('node_publish', 'node_config') AND NOT "
                "shaped.node_online AND shaped.status IN ('queued', 'retrying'), " +
                detail::formatted("shaped.created_at") + ", " +
                detail::formatted("CASE WHEN shaped.status = 'running' THEN shaped.marker_updated END") + ", " +
                detail::formatted("shaped.finished") +
                ", COALESCE((EXTRACT(EPOCH FROM shaped.finished - shaped.created_at) * 1000)::bigint, 0), "
                "shaped.attempts, shaped.failures, shaped.status IN ('completed', 'attention_required') AND "
                "shaped.failures > 0, " +
                detail::formatted("shaped.next_at") +
                ", shaped.error_text, COALESCE(merged.operation_id::text, ''), COALESCE(merged.id::text, ''), "
                "COALESCE(merged_operation.action, ''), COALESCE(merged_operation.target_name, ''), "
                "COALESCE(merged.status, ''), COALESCE(closer.id::text, ''), COALESCE(closer.action, ''), "
                "COALESCE(closer.target_name, '') FROM shaped LEFT JOIN shaped merged ON merged.id = "
                "shaped.merged_into_id LEFT JOIN sys_operation merged_operation ON merged_operation.id = "
                "merged.operation_id LEFT JOIN sys_operation closer ON closer.id = shaped.closed_by_operation_id "
                "WHERE shaped.operation_id = $2::uuid ORDER BY shaped.created_at, shaped.id",
            tenant, id);
        co_await tx.commit();
        if (!subtask_id.empty() && std::none_of(subtasks.begin(), subtasks.end(), [&](const auto& row) {
                return row[0].value().value_or("") == subtask_id;
            })) {
            service::common::throw_app_error(task_error::subtask_not_found);
        }
        operation_detail_dto data({.resource_ = c.pool()});
        assign_operation(data, rows.front(), c.pool());
        auto& list = data.template ensure<"subtasks">();
        for (const auto& row : subtasks) {
            auto& item = list.emplace_back(ruvia::model_options{.resource_ = c.pool()});
            item.template set<"id">(row[0].value().value_or(""));
            item.template set<"parent_id">(row[1].value().value_or(""));
            item.template set<"kind">(row[2].value().value_or(""));
            item.template set<"resource_type">(row[3].value().value_or(""));
            item.template set<"resource_id">(row[4].value().value_or(""));
            item.template set<"resource_name">(row[5].value().value_or(""));
            item.template set<"resource_deleted">(row[6].template as<bool>().value_or(false));
            item.template set<"status">(row[7].value().value_or(""));
            item.template set<"warning">(row[8].value().value_or(""));
            item.template set<"node_offline">(row[9].template as<bool>().value_or(false));
            item.template set<"created_at">(row[10].value().value_or(""));
            item.template set<"running_since">(row[11].value().value_or(""));
            item.template set<"finished_at">(row[12].value().value_or(""));
            item.template set<"duration_ms">(row[13].template as<std::int64_t>().value_or(0));
            item.template set<"attempt_count">(row[14].template as<std::int64_t>().value_or(0));
            item.template set<"failure_count">(row[15].template as<std::int64_t>().value_or(0));
            item.template set<"recovered">(row[16].template as<bool>().value_or(false));
            item.template set<"next_attempt_at">(row[17].value().value_or(""));
            item.template set<"error">(row[18].value().value_or(""));
            if (!row[20].value().value_or("").empty()) {
                subtask_merged_into_dto merged({.resource_ = c.pool()});
                merged.template set<"operation_id">(row[19].value().value_or(""));
                merged.template set<"subtask_id">(row[20].value().value_or(""));
                merged.template set<"action">(row[21].value().value_or(""));
                merged.template set<"target_name">(row[22].value().value_or(""));
                merged.template set<"status">(row[23].value().value_or(""));
                item.template set<"merged_into">(std::move(merged));
            } else if (!row[24].value().value_or("").empty()) {
                subtask_closed_by_dto closer({.resource_ = c.pool()});
                closer.template set<"operation_id">(row[24].value().value_or(""));
                closer.template set<"action">(row[25].value().value_or(""));
                closer.template set<"target_name">(row[26].value().value_or(""));
                item.template set<"closed_by">(std::move(closer));
            }
        }
        co_return data;
    }

    // The newest 100 attempts of one subtask of the task, plus a summary of
    // every retained attempt.
    ruvia::task<subtask_history_dto> history(auto& c, const std::string& tenant, const std::string& id,
        const std::string& subtask_id) {
        subtask_history_dto data({.resource_ = c.pool()});
        // One snapshot keeps the summary consistent with the listed attempts.
        auto tx = co_await c.db().begin_transaction();
        (void)co_await tx.execute("SET TRANSACTION ISOLATION LEVEL REPEATABLE READ, READ ONLY");
        const auto owner = co_await tx.query(
            "SELECT 1 FROM sys_operation_subtask WHERE tenant_id = $1 AND operation_id = $2::uuid AND id = $3::uuid",
            tenant, id, subtask_id);
        if (owner.empty()) {
            co_await tx.commit();
            service::common::throw_app_error(task_error::subtask_not_found);
        }
        const auto rows = co_await tx.query(attempts_sql("") + "ORDER BY id DESC LIMIT $3", tenant, subtask_id,
            static_cast<std::int64_t>(history_limit + 1));
        const auto totals = co_await tx.query(summary_sql(), tenant, subtask_id);
        co_await tx.commit();
        data.template set<"subtask_id">(subtask_id);
        data.template set<"truncated">(rows.size() > history_limit);
        auto& list = data.template ensure<"list">();
        for (const auto& row : rows) {
            if (list.size() == history_limit) {
                break;
            }
            assign_attempt(list.emplace_back(ruvia::model_options{.resource_ = c.pool()}), row, c.pool());
        }
        data.template set<"summary">(read_summary(totals, c.pool()));
        co_return data;
    }

    // `attempt` events (subtask_attempt_event_dto JSON) of the attempts these
    // notices recorded for one subtask, oldest first so a client prepending
    // each one keeps newest-first order. Notices of other subtasks are skipped
    // without a read; attempts already removed by retention are skipped.
    ruvia::task<std::vector<std::string>> recorded_attempts(auto& c, const std::string& tenant,
        const std::string& subtask_id, std::span<const service::live_resource::task_attempt_notice> notices) {
        std::vector<std::int64_t> event_ids;
        for (const auto& notice : notices) {
            if (notice.subtask_id_ == subtask_id &&
                std::find(event_ids.begin(), event_ids.end(), notice.event_id_) == event_ids.end()) {
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
        auto tx = co_await c.db().begin_transaction();
        (void)co_await tx.execute("SET TRANSACTION ISOLATION LEVEL REPEATABLE READ, READ ONLY");
        const auto rows = co_await tx.query(
            attempts_sql("") + "WHERE id = ANY(string_to_array($3, ',')::bigint[]) ORDER BY id", tenant, subtask_id,
            std::string_view(ids));
        const auto totals = co_await tx.query(summary_sql(), tenant, subtask_id);
        co_await tx.commit();
        for (const auto& row : rows) {
            subtask_attempt_event_dto event({.resource_ = c.pool()});
            event.template set<"subtask_id">(subtask_id);
            attempt_dto item({.resource_ = c.pool()});
            assign_attempt(item, row, c.pool());
            event.template set<"item">(std::move(item));
            event.template set<"summary">(read_summary(totals, c.pool()));
            events.emplace_back(ruvia::to_json(event, {.resource_ = c.pool()}));
        }
        co_return events;
    }

    // One attempt of a subtask of the task, with its full trace; an attempt
    // without a stored trace reads as "none".
    ruvia::task<attempt_trace_dto> attempt_trace(auto& c, const std::string& tenant, const std::string& id,
        std::int64_t attempt_id) {
        const auto rows = co_await c.db().query(
            "SELECT numbered.id::text, numbered.subtask_id::text, numbered.resource_type, "
            "numbered.resource_id::text, COALESCE(numbered.detail->>'kind', 'none'), numbered.seq, "
            "numbered.outcome, numbered.error, " +
                detail::formatted("numbered.started_at") + ", " + detail::formatted("numbered.emitted_at") +
                ", numbered.duration_ms, stored.trace->>'worker_type', (stored.trace->'capture')::text, "
                "(stored.trace->'steps')::text FROM (SELECT event.*, row_number() OVER (PARTITION BY "
                "event.subtask_id ORDER BY event.id) AS seq FROM sys_sync_event event INNER JOIN "
                "sys_operation_subtask subtask ON subtask.id = event.subtask_id WHERE event.tenant_id = $1 AND "
                "subtask.operation_id = $2::uuid) numbered LEFT JOIN sys_sync_event_trace stored ON "
                "stored.event_id = numbered.id WHERE numbered.id = $3",
            tenant, id, attempt_id);
        if (rows.empty()) {
            service::common::throw_app_error(task_error::attempt_not_found);
        }
        const auto& row = rows.front();
        attempt_trace_dto data({.resource_ = c.pool()});
        const bool stored = row[13].value().has_value();
        const auto resource_type = std::string(row[2].value().value_or(""));
        data.template set<"attempt_id">(row[0].value().value_or(""));
        data.template set<"subtask_id">(row[1].value().value_or(""));
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

private:
    // Attempts of subtask $2 in tenant $1 numbered from 1, with the stored
    // trace summary and its attempt id.
    static std::string attempts_sql(std::string_view) {
        return "SELECT seq, outcome, error, " + detail::formatted("started_at") + ", " +
               detail::formatted("emitted_at") + ", duration_ms, " + detail::formatted("next_attempt_at") +
               ", detail::text, (COALESCE((SELECT stored.summary FROM sys_sync_event_trace stored WHERE "
               "stored.event_id = numbered.id), '" +
               std::string(service::attempt_trace::summary_none_json) +
               "'::jsonb) || jsonb_build_object('attempt_id', numbered.id::text))::text FROM (SELECT event.*, "
               "row_number() OVER (ORDER BY event.id) AS seq FROM sys_sync_event event WHERE event.tenant_id = $1 "
               "AND event.subtask_id = $2::uuid) numbered ";
    }

    // Aggregates every retained attempt of subtask $2; next_attempt_at is the
    // retry the subtask's marker has scheduled (node work has none).
    static std::string summary_sql() {
        return "SELECT count(*), count(*) FILTER (WHERE outcome = 'failed'), " +
               detail::formatted("min(emitted_at) FILTER (WHERE outcome = 'failed')") + ", " +
               detail::formatted("max(emitted_at) FILTER (WHERE outcome = 'completed')") + ", " +
               detail::formatted(
                   "(SELECT marker.next_attempt_at FROM sys_sync_task marker WHERE marker.tenant_id = $1 AND "
                   "marker.subtask_id = $2::uuid AND NOT marker.is_done AND marker.count_fails > 0 AND "
                   "marker.lease_until IS NULL)") +
               " FROM sys_sync_event WHERE tenant_id = $1 AND subtask_id = $2::uuid";
    }

    template <typename model_type, typename row_type>
    static void assign_operation(model_type& item, const row_type& row, std::pmr::memory_resource* pool) {
        item.template set<"id">(row[0].value().value_or(""));
        item.template set<"action">(row[1].value().value_or(""));
        item.template set<"target_type">(row[2].value().value_or(""));
        item.template set<"target_id">(row[3].value().value_or(""));
        item.template set<"target_name">(row[4].value().value_or(""));
        item.template set<"target_deleted">(row[5].template as<bool>().value_or(false));
        if (const auto kind = row[18].value().value_or(""); !kind.empty()) {
            item.template set<"target_kind">(kind);
        }
        item.template set<"actor_type">(row[6].value().value_or(""));
        item.template set<"actor_name">(row[7].value().value_or(""));
        item.template set<"created_at">(row[8].value().value_or(""));
        item.template set<"updated_at">(row[9].value().value_or(""));
        item.template set<"finished_at">(row[10].value().value_or(""));
        item.template set<"status">(row[11].value().value_or(""));
        item.template set<"subtask_count">(row[12].template as<std::int64_t>().value_or(0));
        item.template set<"settled_count">(row[13].template as<std::int64_t>().value_or(0));
        item.template set<"failed_count">(row[14].template as<std::int64_t>().value_or(0));
        item.template set<"next_attempt_at">(row[15].value().value_or(""));
        item.template set<"error">(row[16].value().value_or(""));
        auto summary = ruvia::json_object::parse(row[17].value().value_or("{}"), {.resource_ = pool});
        if (!summary) {
            summary = ruvia::json_object::parse(std::string_view{"{}"}, {.resource_ = pool});
        }
        item.template set<"summary">(std::move(*summary));
    }

    template <typename row_type>
    static void assign_attempt(attempt_dto& item, const row_type& row, std::pmr::memory_resource* pool) {
        item.template set<"attempt">(row[0].template as<std::int64_t>().value_or(0));
        item.template set<"outcome">(row[1].value().value_or(""));
        item.template set<"error">(row[2].value().value_or(""));
        item.template set<"started_at">(row[3].value().value_or(""));
        item.template set<"emitted_at">(row[4].value().value_or(""));
        item.template set<"duration_ms">(row[5].template as<std::int64_t>().value_or(0));
        item.template set<"next_attempt_at">(row[6].value().value_or(""));
        auto detail = ruvia::json_object::parse(row[7].value().value_or(""), {.resource_ = pool});
        if (!detail) {
            detail = ruvia::json_object::parse(service::sync_runtime::detail_none_json, {.resource_ = pool});
        }
        item.template set<"detail">(std::move(*detail));
        auto trace = ruvia::json_object::parse(row[8].value().value_or(""), {.resource_ = pool});
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
        summary.template set<"next_attempt_at">(total[4].value().value_or(""));
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
