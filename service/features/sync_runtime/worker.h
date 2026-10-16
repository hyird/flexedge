#pragma once

#include <ruvia/core/task.h>
#include <ruvia/core/timer.h>
#include <ruvia/web/db/db_query.h>
#include <ruvia/web/db/db_rows.h>
#include <ruvia/web/db/db_transaction.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "service/features/background/task_context.h"
#include "service/features/background/work_available.h"
#include "service/features/live_resource/notification_outbox.h"
#include "service/features/logging/logger.h"
#include "service/features/sync_runtime/error.h"
#include "service/features/sync_runtime/state.h"

namespace service::sync_runtime {

// Every marker loop scans its expired leases and stale waits this often,
// re-derives missing markers from its aggregates this often, and waits this
// long for its own wakeup when no marker was due.
inline constexpr std::chrono::seconds lease_recovery_interval{15};
inline constexpr std::chrono::minutes reconciliation_interval{15};
inline constexpr std::chrono::seconds idle_wait{15};
// Attempts older than this leave the task history; one claimer per interval
// prunes them for the whole deployment.
inline constexpr std::chrono::hours retention_interval{1};
inline constexpr std::string_view retention_job{"sync_event_retention"};

// A marker this worker leased: its attempt starts at `claimed_at_`. `row_` is
// the claimed row; the aggregate columns the claim asked for are read from it
// by name.
struct claimed_marker final {
    std::string id_;
    std::string tenant_id_;
    std::string resource_id_;
    marker_operation operation_{};
    std::int64_t version_{};
    std::int64_t failures_{};
    std::chrono::steady_clock::time_point claimed_at_{};
    ruvia::db_rows row_;

    [[nodiscard]] const ruvia::db_row& columns() const& noexcept {
        return row_.front();
    }
};

// Leases the next due marker of `resource_type` to this worker: one row, the
// oldest due first, skipping rows another claimer holds. `aggregate_columns`
// of the marker's aggregate row come back with the claim.
inline ruvia::task<std::optional<claimed_marker>> claim_marker(service::background::task_context& context,
    marker_resource_type resource_type, std::span<const std::string_view> aggregate_columns = {}) {
    const auto& type = describe(resource_type);
    ruvia::db_query candidate;
    const auto marker = [&](std::string_view column) { return candidate.column(column, "marker"); };
    const auto subject = [&](std::string_view column) { return candidate.column(column, "subject"); };
    const auto equals = [&](ruvia::db_expression left, ruvia::db_expression right) {
        return candidate.binary(left, ruvia::db_binary_operator::equal, right);
    };
    std::vector<ruvia::db_expression> selected{marker("id")};
    for (const auto column : aggregate_columns) {
        selected.push_back(candidate.alias(subject(column), column));
    }
    auto live_subject = candidate.unary(ruvia::db_unary_operator::is_null, subject("deleted_at"));
    if (type.deleted_operation_) {
        live_subject = candidate.binary(live_subject, ruvia::db_binary_operator::or_value,
            equals(marker("operation"), candidate.value(marker_operation_name(*type.deleted_operation_))));
    }
    candidate.select(selected)
        .from("sys_sync_task", "marker")
        .join(ruvia::db_join_type::inner, type.table_,
            candidate.binary(equals(subject("tenant_id"), marker("tenant_id")), ruvia::db_binary_operator::and_value,
                equals(subject("id"), marker("resource_id"))),
            "subject")
        .where(equals(marker("resource_type"), candidate.value(type.name_)))
        .and_where(candidate.unary(ruvia::db_unary_operator::not_value, marker("is_done")))
        .and_where(candidate.binary(marker("next_attempt_at"), ruvia::db_binary_operator::less_equal,
            candidate.call("now")))
        .and_where(candidate.unary(ruvia::db_unary_operator::is_null, marker("lease_until")))
        .and_where(candidate.unary(ruvia::db_unary_operator::is_null, marker("wait_task_id")))
        .and_where(equals(marker("version"), subject(type.version_column_)))
        .and_where(live_subject)
        .order_by(marker("next_attempt_at"))
        .add_order_by(marker("updated_at"))
        .limit(1)
        .lock({.mode_ = ruvia::db_row_lock::update, .skip_locked_ = true, .tables_ = {"marker"}});

    ruvia::db_query claim;
    const auto claimed = [&](std::string_view column) { return claim.column(column, "marker"); };
    std::vector<ruvia::db_expression> returned{claimed("id"), claimed("tenant_id"), claimed("resource_id"),
        claimed("operation"), claimed("version"), claimed("count_fails")};
    for (const auto column : aggregate_columns) {
        returned.push_back(claim.column(column, "candidate"));
    }
    claim.with("candidate", candidate)
        .update("sys_sync_task", "marker")
        .set("lease_owner", claim.value(context.lease_owner()))
        .set("lease_until", detail::now_plus(claim, lease_duration))
        .set("updated_at", claim.call("now"))
        .update_from("candidate")
        .where(claim.binary(claimed("id"), ruvia::db_binary_operator::equal, claim.column("id", "candidate")))
        .returning(returned);

    auto transaction = co_await context.db().begin_transaction();
    auto rows = co_await transaction.query(claim);
    if (rows.empty()) {
        co_await transaction.commit();
        co_return std::nullopt;
    }
    const auto claimed_at = std::chrono::steady_clock::now();
    const auto& row = rows.front();
    claimed_marker result{
        .id_ = std::string(row[0].value().value_or("")),
        .tenant_id_ = std::string(row[1].value().value_or("")),
        .resource_id_ = std::string(row[2].value().value_or("")),
        .operation_ = require_marker_operation(row[3].value().value_or("")),
        .version_ = row[4].as<std::int64_t>().value_or(1),
        .failures_ = row[5].as<std::int64_t>().value_or(0),
        .claimed_at_ = claimed_at,
        .row_ = std::move(rows),
    };
    service::live_resource::notifications::resource_notification notification(result.tenant_id_);
    notification.changed(service::live_resource::resource::tasks, result.resource_id_);
    if (type.claim_changes_resource_) {
        notification.changed(type.resource_, result.resource_id_);
    }
    co_await service::live_resource::commit_and_notify(transaction, context, notification);
    co_return std::move(result);
}

namespace detail {

// Notifies the changes of several tenants after one autocommit statement:
// one notification per tenant.
template <typename context_type>
inline ruvia::task<void> commit_tenant_changes(ruvia::db_transaction& transaction, context_type& context,
    std::vector<service::live_resource::notifications::resource_notification> notifications) {
    for (const auto& notification : notifications) {
        (void)co_await service::live_resource::enqueue_notification(transaction, notification);
    }
    co_await transaction.commit();
    if constexpr (requires { context.record_committed_notification(notifications.front()); }) {
        for (const auto& notification : notifications) {
            co_await context.record_committed_notification(notification);
        }
    }
}

inline service::live_resource::notifications::resource_notification& tenant_notification(
    std::vector<service::live_resource::notifications::resource_notification>& notifications,
    std::string_view tenant_id) {
    for (auto& notification : notifications) {
        if (notification.tenant_id_ == tenant_id) {
            return notification;
        }
    }
    return notifications.emplace_back(tenant_id);
}

// Commits a statement that changed markers of `resource_type` and returned
// (tenant_id, resource_id) rows.
template <typename context_type>
inline ruvia::task<void> commit_marker_changes(ruvia::db_transaction& transaction, context_type& context,
    marker_resource_type resource_type, const ruvia::db_rows& rows) {
    std::vector<service::live_resource::notifications::resource_notification> notifications;
    for (const auto& row : rows) {
        marker_changed(tenant_notification(notifications, row[0].value().value_or("")), resource_type,
            row[1].value().value_or(""));
    }
    co_await commit_tenant_changes(transaction, context, std::move(notifications));
}

}  // namespace detail

// Expired leases of `resource_type` (their claimer stopped or stalled) count
// as failed attempts and retry.
template <typename context_type>
inline ruvia::task<void> recover_stale_running(context_type& context, marker_resource_type resource_type) {
    auto transaction = co_await context.db().begin_transaction();
    const auto rows = co_await transaction.query(
        "SELECT tenant_id, id, resource_id, version, lease_owner FROM sys_sync_task "
        "WHERE resource_type = $1 AND lease_until <= NOW() "
        "ORDER BY lease_until, id LIMIT 32 FOR UPDATE SKIP LOCKED",
        describe(resource_type).name_);
    std::vector<service::live_resource::notifications::resource_notification> notifications;
    for (const auto& row : rows) {
        const auto lease = make_running_lease(row[0].value().value(), row[1].value().value(),
            row[3].template as<std::int64_t>().value(), row[4].value().value(), {},
            resource_type, row[2].value().value());
        (void)co_await transaction.execute(
            "UPDATE sys_sync_task SET is_done = FALSE, is_ok = FALSE, count_fails = count_fails + 1, "
            "error = $2, next_attempt_at = NOW() + CAST($3 AS BIGINT) * INTERVAL '1 second', "
            "lease_owner = NULL, lease_until = NULL, updated_at = NOW() WHERE id = $1",
            lease.marker_.marker_id_, std::string_view{lease_recovery_retry_error}, retry_delay_seconds);
        const auto event_id = co_await record_running_result_event(transaction, lease,
            attempt_outcome::retryable_failure, detail_none_json);
        if (!event_id) {
            throw std::runtime_error("recovered sync marker result was not recorded");
        }
        auto& notification = detail::tenant_notification(notifications, lease.marker_.tenant_id_);
        detail::marker_changed(notification, resource_type, lease.marker_.resource_id_);
        notification.task_attempt(lease.marker_.resource_id_, *event_id);
    }
    co_await detail::commit_tenant_changes(transaction, context, std::move(notifications));
}

// Waiters of `resource_type` whose awaited task is gone or already processed
// the awaited version resume (a missed resumption, e.g. a removal).
template <typename context_type>
inline ruvia::task<void> resume_stale_waiters(context_type& context, marker_resource_type resource_type) {
    auto transaction = co_await context.db().begin_transaction();
    const auto rows = co_await transaction.query(
        "UPDATE sys_sync_task waiter SET wait_task_id = NULL, wait_version = NULL, "
        "next_attempt_at = NOW() + make_interval(secs => $2), updated_at = NOW() "
        "WHERE waiter.resource_type = $1 AND waiter.wait_task_id IS NOT NULL AND NOT EXISTS "
        "(SELECT 1 FROM sys_sync_task awaited WHERE awaited.tenant_id = waiter.tenant_id AND "
        "awaited.id = waiter.wait_task_id AND awaited.processed_version < waiter.wait_version) "
        "RETURNING waiter.tenant_id, waiter.resource_id",
        describe(resource_type).name_, std::int64_t{wait_settle_delay.count()});
    co_await detail::commit_marker_changes(transaction, context, resource_type, rows);
}

// Retention: attempts older than 7 days (their traces cascade). Runs once per
// retention_interval for the deployment: the claimer that advances the due
// job row prunes, every other caller finds it not due or locked and skips.
// Open histories of the affected tasks resend once; lists re-read.
template <typename context_type>
inline ruvia::task<void> prune_result_events(context_type& context) {
    auto transaction = co_await context.db().begin_transaction();
    const auto due = co_await transaction.query(
        "WITH due AS (SELECT name FROM sys_scheduled_job WHERE name = $1 AND next_run_at <= NOW() "
        "FOR UPDATE SKIP LOCKED) UPDATE sys_scheduled_job job SET next_run_at = NOW() + "
        "make_interval(secs => $2) FROM due WHERE job.name = due.name RETURNING job.name",
        retention_job, std::int64_t{std::chrono::seconds(retention_interval).count()});
    if (due.empty()) {
        co_await transaction.commit();
        co_return;
    }
    const auto removed = co_await transaction.query(
        "WITH removed AS (DELETE FROM sys_sync_event WHERE emitted_at < NOW() - INTERVAL '7 days' "
        "RETURNING tenant_id, resource_id) SELECT DISTINCT tenant_id::text, resource_id::text FROM removed");
    std::vector<service::live_resource::notifications::resource_notification> notifications;
    for (const auto& row : removed) {
        detail::tenant_notification(notifications, row[0].value().value_or(""))
            .task_history_reset(row[1].value().value_or(""));
    }
    co_await detail::commit_tenant_changes(transaction, context, std::move(notifications));
}

// The claim loop of one marker type, the same on every worker: recovers
// expired leases and stale waits, reconciles missing markers
// (`reconcile(context)`), runs the retention when due, then claims and runs up
// to claims_per_round_ markers (`process(context, const claimed_marker&)`),
// claiming `aggregate_columns` with each. With nothing due it waits for its
// own wakeup or the next bounded scan.
template <typename reconcile_type, typename process_type>
ruvia::task<void> run_marker_worker(service::background::task_context& context, marker_resource_type resource_type,
    reconcile_type reconcile, process_type process, std::span<const std::string_view> aggregate_columns = {},
    std::chrono::seconds maintenance_interval = reconciliation_interval) {
    const auto& type = describe(resource_type);
    const auto stopped = [&] { return context.get_stop_token().stop_requested(); };
    // Subscribe before the first durable claim: a wakeup committed meanwhile
    // stays queued until the loop waits.
    auto own = service::background::work_available::subscribe(context, type.work_type_);
    auto next_recovery = std::chrono::steady_clock::now();
    auto next_reconciliation = next_recovery;
    auto next_retention = next_recovery;
    while (!stopped()) {
        std::string failure;
        std::size_t processed = 0;
        try {
            if (std::chrono::steady_clock::now() >= next_recovery) {
                co_await recover_stale_running(context, resource_type);
                co_await resume_stale_waiters(context, resource_type);
                next_recovery = std::chrono::steady_clock::now() + lease_recovery_interval;
            }
            if (stopped()) {
                break;
            }
            if (std::chrono::steady_clock::now() >= next_reconciliation) {
                co_await reconcile(context);
                next_reconciliation = std::chrono::steady_clock::now() + maintenance_interval;
            }
            if (stopped()) {
                break;
            }
            if (std::chrono::steady_clock::now() >= next_retention) {
                co_await prune_result_events(context);
                next_retention = std::chrono::steady_clock::now() + retention_interval;
            }
            for (; processed < type.claims_per_round_ && !stopped(); ++processed) {
                const auto marker = co_await claim_marker(context, resource_type, aggregate_columns);
                if (!marker || stopped()) {
                    break;
                }
                co_await process(context, *marker);
            }
        } catch (const std::exception& error) {
            failure = worker_error(error.what());
        } catch (...) {
            failure = std::string(type.unknown_failure_);
        }
        if (stopped()) {
            break;
        }
        if (!failure.empty()) {
            service::logging::error(std::string(type.worker_label_) + " failure: " + failure);
        }
        if (processed == 0 && failure.empty()) {
            try {
                (void)co_await own.receive_for(idle_wait, context.get_stop_token());
            } catch (const std::exception&) {
                if (stopped()) {
                    break;
                }
                failure = "task wakeup unavailable";
            }
        }
        if (!failure.empty() && co_await ruvia::sleep_for(context.worker(), std::chrono::seconds(2),
                                    context.get_stop_token()) == ruvia::timer_sleep_result::stop_requested) {
            break;
        }
    }
}

}  // namespace service::sync_runtime
