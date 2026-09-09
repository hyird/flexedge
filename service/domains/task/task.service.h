#pragma once

#include <algorithm>
#include <cstdint>
#include <string>

#include <ruvia/core/Task.h>
#include <ruvia/web/Context.h>
#include "service/domains/task/task.types.h"
#include "service/domains/task/task.error.h"

namespace service::task {

// Markers remain coalesced scheduling state. The read model groups retained
// results by version; it never changes scheduling or treats an old failed
// attempt as the state of a newer version. Every branch is tenant-scoped.
inline const std::string kTaskRecordsSql = R"sql(
WITH history AS (
    SELECT task_id, resource_type, resource_id, version,
        (array_agg(operation ORDER BY id DESC))[1] AS operation,
        (array_agg(outcome ORDER BY id DESC))[1] AS outcome,
        (array_agg(error ORDER BY id DESC))[1] AS error,
        count(*) FILTER (WHERE outcome = 'failed') AS failures,
        max(emitted_at) AS updated_at
    FROM sys_sync_event WHERE tenant_id = $1
    GROUP BY task_id, resource_type, resource_id, version
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
        CASE WHEN t.status = 'applied' THEN 'completed'
            WHEN r.status = 'superseded' OR t.status = 'excluded' THEN 'superseded'
            WHEN t.status = 'failed' AND t.retryable IS TRUE THEN 'retrying'
            WHEN t.status = 'failed' THEN 'failed' ELSE 'queued' END,
        COALESCE(t.last_error, ''), CASE WHEN t.status = 'failed' THEN 1 ELSE 0 END,
        t.updated_at, NULL::timestamptz
    FROM sys_node_release_target t JOIN sys_cluster_release r
        ON r.tenant_id = t.tenant_id AND r.id = t.release_id
    JOIN sys_node n ON n.tenant_id = t.tenant_id AND n.id = t.node_id
    WHERE t.tenant_id = $1 AND (r.status <> 'superseded' OR t.updated_at >= NOW() - INTERVAL '7 days')
), filtered AS (
    SELECT * FROM records WHERE ($2 = '' OR resource_type = $2)
        AND ($3 = '' OR status = $3)
        AND ($4 = '' OR strpos(lower(name), lower($4)) > 0 OR strpos(resource_id::text, $4) > 0)
        AND ($5::integer = 0 OR updated_at >= NOW() - $5::integer * INTERVAL '1 day')
)
)sql";

class TaskService final {
  public:
    ruvia::Task<TaskDto> detail(auto& c, const std::string& tenant, const std::string& id,
                                const std::string& resourceId, std::int64_t version) {
        const auto rows = co_await c.db().query(
            kTaskRecordsSql + taskColumns() +
                "FROM filtered WHERE id = $6 AND resource_id = $7 AND version = $8",
            tenant, std::string_view{}, std::string_view{}, std::string_view{}, 0, id, resourceId,
            version);
        if (rows.empty())
            service::common::throwAppError(TaskError::NOT_FOUND);
        TaskDto item(c);
        assign(item, rows.front());
        co_return item;
    }

    ruvia::Task<TaskPageDto> list(auto& c, const std::string& tenant, std::int64_t page,
                                  std::int64_t pageSize, std::int64_t skip, const std::string& type,
                                  const std::string& status, const std::string& keyword,
                                  std::int64_t days) {
        TaskPageDto data(c);
        // One snapshot keeps the totals and rows consistent during rapid results.
        auto tx = co_await c.db().beginTransaction();
        (void)co_await tx.execute("SET TRANSACTION ISOLATION LEVEL REPEATABLE READ, READ ONLY");
        const auto counts = co_await tx.query(
            kTaskRecordsSql +
                "SELECT (SELECT count(*) FROM filtered), "
                "count(*) FILTER (WHERE status IN ('queued', 'running', 'retrying')), "
                "count(*) FILTER (WHERE status = 'failed') FROM records",
            tenant, type, status, keyword, days);
        const auto total = counts.front()[0].template as<std::int64_t>().value_or(0);
        data.template set<"total">(total);
        data.template set<"page">(page);
        data.template set<"pageSize">(pageSize);
        data.template set<"totalPages">((total + pageSize - 1) / pageSize);
        data.template set<"active">(counts.front()[1].template as<std::int64_t>().value_or(0));
        data.template set<"failed">(counts.front()[2].template as<std::int64_t>().value_or(0));
        const auto rows = co_await tx.query(kTaskRecordsSql + taskColumns() +
                                                "FROM filtered ORDER BY updated_at DESC, id, "
                                                "resource_id, version DESC LIMIT $6 OFFSET $7",
                                            tenant, type, status, keyword, days, pageSize, skip);
        auto& list = data.template ensure<"list">();
        for (const auto& row : rows) {
            auto& item = list.emplace_back(c);
            assign(item, row);
        }
        co_await tx.commit();
        co_return data;
    }

    ruvia::Task<TaskHistoryDto> history(auto& c, const std::string& tenant,
                                        const std::string& id, std::int64_t version) {
        const auto rows = co_await c.db().query(
            "SELECT outcome, error, TO_CHAR(emitted_at, 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF') "
            "FROM sys_sync_event WHERE tenant_id = $1 AND task_id = $2 AND version = $3 "
            "ORDER BY id DESC LIMIT 101",
            tenant, id, version);
        TaskHistoryDto data(c);
        data.template set<"truncated">(rows.size() > 100);
        auto& list = data.template ensure<"list">();
        std::size_t count = 0;
        for (const auto& row : rows) {
            if (count++ == 100)
                break;
            auto& item = list.emplace_back(c);
            item.template set<"outcome">(row[0].value().value_or(""));
            item.template set<"error">(row[1].value().value_or(""));
            item.template set<"emittedAt">(row[2].value().value_or(""));
        }
        co_return data;
    }

  private:
    static std::string taskColumns() {
        return "SELECT id, resource_type, resource_id, name, operation, version, status, error, "
               "failures, "
               "TO_CHAR(updated_at, 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF'), "
               "COALESCE(TO_CHAR(next_attempt_at, 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF'), '') ";
    }
    template <typename Row> static void assign(TaskDto& item, const Row& row) {
        item.template set<"id">(row[0].value().value_or(""));
        item.template set<"resourceType">(row[1].value().value_or(""));
        item.template set<"resourceId">(row[2].value().value_or(""));
        item.template set<"name">(row[3].value().value_or(""));
        item.template set<"operation">(row[4].value().value_or(""));
        item.template set<"version">(row[5].template as<std::int64_t>().value_or(0));
        item.template set<"status">(row[6].value().value_or(""));
        item.template set<"error">(row[7].value().value_or(""));
        item.template set<"failures">(row[8].template as<std::int64_t>().value_or(0));
        item.template set<"updatedAt">(row[9].value().value_or(""));
        item.template set<"nextAttemptAt">(row[10].value().value_or(""));
    }
};

inline TaskService& taskService() {
    static TaskService service;
    return service;
}
} // namespace service::task
