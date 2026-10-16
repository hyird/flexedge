#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/db/db_query.h>
#include <ruvia/web/db/db_transaction.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "service/features/attempt_trace/recorder.h"
#include "service/features/attempt_trace/trace_store.h"
#include "service/features/live_resource/notification_outbox.h"
#include "service/features/operation/work_origin.h"
#include "service/features/sync_runtime/attempt_record.h"

namespace service::sync_runtime {

enum class marker_resource_type {
    provider,
    dns_zone,
    certificate,
    website,
};

enum class marker_operation {
    verify,
    sync,
    sync_local,
    sync_remote,
    remove,
    issue,
    renew,
    apply,
};

[[nodiscard]] inline constexpr std::string_view resource_type_name(marker_resource_type resource_type) {
    switch (resource_type) {
        case marker_resource_type::provider:
            return "provider";
        case marker_resource_type::dns_zone:
            return "dns_zone";
        case marker_resource_type::certificate:
            return "certificate";
        case marker_resource_type::website:
            return "website";
    }
    throw std::invalid_argument("unsupported sync marker resource type");
}

[[nodiscard]] inline constexpr std::string_view marker_operation_name(marker_operation operation) {
    switch (operation) {
        case marker_operation::verify:
            return "verify";
        case marker_operation::sync:
            return "sync";
        case marker_operation::sync_local:
            return "sync_local";
        case marker_operation::sync_remote:
            return "sync_remote";
        case marker_operation::remove:
            return "delete";
        case marker_operation::issue:
            return "issue";
        case marker_operation::renew:
            return "renew";
        case marker_operation::apply:
            return "apply";
    }
    throw std::invalid_argument("unsupported sync marker operation");
}

[[nodiscard]] inline std::optional<marker_operation>
parse_marker_operation(std::string_view operation) {
    if (operation == "verify") {
        return marker_operation::verify;
    }
    if (operation == "sync") {
        return marker_operation::sync;
    }
    if (operation == "sync_local") {
        return marker_operation::sync_local;
    }
    if (operation == "sync_remote") {
        return marker_operation::sync_remote;
    }
    if (operation == "delete") {
        return marker_operation::remove;
    }
    if (operation == "issue") {
        return marker_operation::issue;
    }
    if (operation == "renew") {
        return marker_operation::renew;
    }
    if (operation == "apply") {
        return marker_operation::apply;
    }
    return std::nullopt;
}

[[nodiscard]] inline marker_operation require_marker_operation(std::string_view operation) {
    const auto parsed = parse_marker_operation(operation);
    if (!parsed) {
        throw std::invalid_argument("invalid sync marker operation");
    }
    return *parsed;
}

[[nodiscard]] inline constexpr bool supports_marker_operation(marker_resource_type resource_type,
    marker_operation operation) {
    switch (resource_type) {
        case marker_resource_type::provider:
            return operation == marker_operation::verify;
        case marker_resource_type::dns_zone:
            return operation == marker_operation::sync || operation == marker_operation::sync_local ||
                   operation == marker_operation::sync_remote || operation == marker_operation::remove;
        case marker_resource_type::certificate:
            return operation == marker_operation::issue || operation == marker_operation::renew;
        case marker_resource_type::website:
            return operation == marker_operation::apply;
    }
    return false;
}

// The subtask kind doing a marker's work.
[[nodiscard]] inline service::operation::subtask_kind marker_subtask_kind(marker_resource_type resource_type,
    marker_operation operation, service::operation::subtask_purpose purpose) {
    using service::operation::subtask_kind;
    switch (resource_type) {
        case marker_resource_type::provider:
            return subtask_kind::provider_verify;
        case marker_resource_type::dns_zone:
            if (operation == marker_operation::remove) {
                return subtask_kind::dns_delete;
            }
            if (purpose == service::operation::subtask_purpose::dns_challenge) {
                return subtask_kind::dns_challenge;
            }
            if (purpose == service::operation::subtask_purpose::dns_challenge_cleanup) {
                return subtask_kind::dns_challenge_cleanup;
            }
            return subtask_kind::dns_sync;
        case marker_resource_type::certificate:
            return operation == marker_operation::renew ? subtask_kind::certificate_renew
                                                        : subtask_kind::certificate_issue;
        case marker_resource_type::website:
            return subtask_kind::website_check;
    }
    throw std::invalid_argument("unsupported sync marker resource type");
}

// RETURNING columns of a claim: the claimed marker's subtask and task, empty
// for unowned work. `alias` names the marker table in the claim statement.
[[nodiscard]] inline std::string claimed_owner_columns(std::string_view alias) {
    const auto marker = std::string(alias);
    return "COALESCE(" + marker +
           ".subtask_id::text, ''), COALESCE((SELECT owner.operation_id::text FROM "
           "sys_operation_subtask owner WHERE owner.id = " +
           marker + ".subtask_id), '')";
}

template <typename row_type>
[[nodiscard]] service::operation::work_owner claimed_owner(const row_type& row, std::size_t subtask_column) {
    return {.operation_id_ = std::string(row[subtask_column + 1].value().value_or("")),
        .subtask_id_ = std::string(row[subtask_column].value().value_or(""))};
}

// A sync task is a coalesced, current-state marker. It is deliberately not a
// historical workflow node: the aggregate remains the source of truth and
// version fences decide whether a worker result is still current.
struct marker_reference final {
    std::string_view tenant_id_;
    std::string_view marker_id_;
    std::int64_t version_{};
    marker_resource_type resource_type_{};
    std::string_view resource_id_;
};

// `started_` is the monotonic instant the claim acquired the lease; it times
// the attempt for the task history.
struct running_marker_lease final {
    marker_reference marker_;
    std::string_view owner_;
    std::chrono::steady_clock::time_point started_{};
};

// `event_id_` identifies the recorded attempt row when one was written;
// `owner_` is the subtask (and task) it was recorded for, empty for work that
// belongs to no task.
struct running_result_transition final {
    bool marker_transitioned_{};
    bool event_recorded_{};
    std::optional<std::int64_t> event_id_;
    service::operation::work_owner owner_{};
};

inline constexpr std::int64_t retry_delay_seconds{15};
inline constexpr std::int64_t lease_timeout_seconds{60};
inline constexpr std::string_view lease_recovery_retry_error{"同步租约超时，已回收重试"};

[[nodiscard]] inline running_marker_lease make_running_lease(std::string_view tenant_id,
    std::string_view marker_id,
    std::int64_t version,
    std::string_view owner,
    std::chrono::steady_clock::time_point started,
    marker_resource_type resource_type = marker_resource_type::website,
    std::string_view resource_id = {}) {
    if (tenant_id.empty() || marker_id.empty() || owner.empty() || version <= 0) {
        throw std::invalid_argument("invalid running marker lease");
    }
    return {.marker_ = {.tenant_id_ = tenant_id, .marker_id_ = marker_id, .version_ = version, .resource_type_ = resource_type, .resource_id_ = resource_id},
        .owner_ = owner,
        .started_ = started};
}

namespace detail {

inline service::live_resource::resource resource_for_marker_type(marker_resource_type resource_type) {
    using service::live_resource::resource;
    switch (resource_type) {
        case marker_resource_type::provider:
            return resource::providers;
        case marker_resource_type::dns_zone:
            return resource::dns_zones;
        case marker_resource_type::certificate:
            return resource::certificates;
        case marker_resource_type::website:
            return resource::websites;
    }
    return resource::operations;
}

// The committed result of one attempt: its task (with the recorded attempt
// when there is one) and the marker's resource.
inline service::live_resource::notifications::resource_notification recorded_result_notification(
    const running_marker_lease& lease, const running_result_transition& transition) {
    service::live_resource::notifications::resource_notification notification(lease.marker_.tenant_id_);
    if (transition.event_id_ && !transition.owner_.empty()) {
        notification.task_attempt(transition.owner_.operation_id_,
            {.subtask_id_ = transition.owner_.subtask_id_, .event_id_ = *transition.event_id_});
    } else if (!transition.owner_.operation_id_.empty()) {
        notification.changed(service::live_resource::resource::operations, transition.owner_.operation_id_);
    }
    notification.changed(resource_for_marker_type(lease.marker_.resource_type_), lease.marker_.resource_id_);
    return notification;
}

}  // namespace detail

// Commits the result, then stores the attempt trace with the worker's
// autocommit handle (a trace failure is logged and never undoes or delays the
// committed result), then notifies the change on the worker's own Redis
// connection (a notification failure is logged as well). `node_config`: the
// transaction may have changed node desired state (certificate material).
// `caused`: work the result scheduled; the tasks it changed are notified.
template <typename transaction_type, typename context_type>
inline ruvia::task<void> commit_and_publish_result_event(transaction_type& transaction, context_type& context,
    const running_marker_lease& lease, const running_result_transition& transition,
    service::attempt_trace::recorder& trace, bool node_config = false,
    const service::operation::work_origin* caused = nullptr) {
    auto notification = transition.event_recorded_ || !transition.owner_.operation_id_.empty()
                            ? detail::recorded_result_notification(lease, transition)
                            : service::live_resource::notifications::resource_notification(lease.marker_.tenant_id_);
    if (node_config) {
        notification.node_config();
    }
    if (caused != nullptr) {
        caused->add_changes(notification);
    }
    const auto notification_id = co_await service::live_resource::enqueue_notification(
        transaction, notification, service::live_resource::trace_delivery_guard);
    co_await transaction.commit();
    if (transition.event_id_) {
        auto database = context.db();
        co_await service::attempt_trace::store_attempt_trace(database,
            {.tenant_id_ = lease.marker_.tenant_id_,
                .task_id_ = lease.marker_.marker_id_,
                .resource_id_ = lease.marker_.resource_id_,
                .version_ = lease.marker_.version_},
            *transition.event_id_, trace);
    }
    co_await service::live_resource::release_notification(context, notification_id, notification);
}

[[nodiscard]] inline std::string_view resource_column(marker_resource_type resource_type) {
    switch (resource_type) {
        case marker_resource_type::provider:
            return "provider_id";
        case marker_resource_type::dns_zone:
            return "dns_zone_id";
        case marker_resource_type::certificate:
            return "certificate_id";
        case marker_resource_type::website:
            return "website_id";
    }
    throw std::invalid_argument("unsupported sync marker resource type");
}

// Queues work on a resource at `version` for the subtask `origin` names.
// The marker coalesces: a newer version replaces pending work, whose subtask
// is settled as merged into the new owner (or completed when it already ran),
// and an older version merges into the current owner. Runs under the marker
// row lock, so ownership changes are serialized with claims and results.
inline ruvia::task<std::string> upsert_marker(ruvia::db_transaction& transaction,
    std::string_view tenant_id,
    marker_resource_type resource_type,
    std::string_view resource_id, marker_operation operation,
    std::int64_t version, service::operation::work_origin& origin) {
    using service::operation::work_origin;
    if (version <= 0) {
        throw std::invalid_argument("sync marker version must be positive");
    }
    if (!supports_marker_operation(resource_type, operation)) {
        throw std::invalid_argument("sync marker operation is not valid for this resource");
    }
    const auto column = resource_column(resource_type);
    const auto resource_type_value = resource_type_name(resource_type);
    const auto operation_value = marker_operation_name(operation);
    const auto existing = co_await transaction.query(
        "SELECT marker.version, marker.is_done AND marker.is_ok, COALESCE(marker.subtask_id::text, ''), "
        "COALESCE(subtask.operation_id::text, '') FROM sys_sync_task marker LEFT JOIN "
        "sys_operation_subtask subtask ON subtask.id = marker.subtask_id WHERE marker.tenant_id = $1 AND "
        "marker.resource_type = $2 AND marker." +
            std::string(column) + " = $3 FOR UPDATE OF marker",
        tenant_id, resource_type_value, resource_id);
    const bool exists = !existing.empty();
    const auto existing_version = exists ? existing.front()[0].as<std::int64_t>().value_or(0) : 0;
    const bool existing_ran = exists && existing.front()[1].as<bool>().value_or(false);
    const auto existing_subtask = exists ? std::string(existing.front()[2].value().value_or("")) : std::string{};
    const auto existing_operation = exists ? std::string(existing.front()[3].value().value_or("")) : std::string{};

    std::string owner;
    if (origin.get_mode() == work_origin::mode::repair) {
        if (exists && existing_version == version) {
            owner = existing_subtask;
        } else {
            const auto reopened = co_await service::operation::reopen_subtask_of_version(
                transaction, tenant_id, resource_type_value, resource_id, version);
            owner = reopened.subtask_id_;
            origin.touch(reopened.operation_id_);
        }
    } else {
        owner = (co_await origin.require_subtask(transaction, tenant_id,
                     marker_subtask_kind(resource_type, operation, origin.purpose()), resource_type_value,
                     resource_id, version))
                    .value_or("");
    }

    const auto rows = co_await transaction.query(
        "INSERT INTO sys_sync_task (tenant_id, resource_type, " + std::string(column) +
            ", operation, version, processed_version, is_done, is_ok, error, count_fails, "
            "next_attempt_at, created_at, updated_at, subtask_id) VALUES ($1, $2, $3, $4, $5, 0, FALSE, "
            "FALSE, '', 0, NOW(), NOW(), NOW(), NULLIF($6, '')::uuid) ON CONFLICT (tenant_id, resource_type, "
            "resource_id) DO UPDATE SET version = GREATEST(sys_sync_task.version, "
            "EXCLUDED.version), operation = CASE WHEN EXCLUDED.version >= sys_sync_task.version "
            "THEN EXCLUDED.operation ELSE sys_sync_task.operation END, is_done = CASE WHEN "
            "EXCLUDED.version >= sys_sync_task.version THEN FALSE ELSE sys_sync_task.is_done END, "
            "is_ok = CASE WHEN EXCLUDED.version >= sys_sync_task.version THEN FALSE ELSE "
            "sys_sync_task.is_ok END, processed_version = CASE WHEN EXCLUDED.version >= "
            "sys_sync_task.version THEN 0 ELSE sys_sync_task.processed_version END, error = CASE "
            "WHEN EXCLUDED.version >= sys_sync_task.version THEN '' ELSE sys_sync_task.error END, "
            "count_fails = CASE WHEN EXCLUDED.version >= sys_sync_task.version THEN 0 ELSE "
            "sys_sync_task.count_fails END, next_attempt_at = CASE WHEN EXCLUDED.version >= "
            "sys_sync_task.version THEN NOW() ELSE sys_sync_task.next_attempt_at END, updated_at = "
            "CASE WHEN EXCLUDED.version >= sys_sync_task.version THEN NOW() ELSE "
            "sys_sync_task.updated_at END, lease_owner = CASE WHEN EXCLUDED.version > "
            "sys_sync_task.version THEN NULL ELSE sys_sync_task.lease_owner END, lease_until = "
            "CASE WHEN EXCLUDED.version > sys_sync_task.version THEN NULL ELSE "
            "sys_sync_task.lease_until END, subtask_id = CASE WHEN EXCLUDED.version >= "
            "sys_sync_task.version THEN EXCLUDED.subtask_id ELSE sys_sync_task.subtask_id END RETURNING id",
        tenant_id, resource_type_value, resource_id, operation_value, version, std::string_view(owner));
    if (rows.empty()) {
        throw std::runtime_error("sync marker could not be created");
    }
    const auto worker_type = resource_type == marker_resource_type::provider ? "provider-verification" :
        resource_type == marker_resource_type::dns_zone ? "dns-sync" :
        resource_type == marker_resource_type::certificate ? "certificate" : "website-dispatch";
    co_await service::live_resource::enqueue_worker_wakeup(transaction, worker_type);
    const auto closed_by = origin.operation_id().value_or("");
    if (exists && version >= existing_version && !existing_subtask.empty() && existing_subtask != owner) {
        // Work that already ran keeps its result; pending or running work is
        // taken over by the new owner (an equal version keeps a running
        // attempt, whose result then belongs to the new owner).
        const auto settled = existing_ran
                                 ? co_await service::operation::settle_subtask(transaction, tenant_id,
                                       existing_subtask, service::operation::state_completed)
                                 : co_await service::operation::settle_subtask(transaction, tenant_id,
                                       existing_subtask, service::operation::state_merged, owner, closed_by);
        origin.touch(settled.value_or(existing_operation));
    }
    if (exists && version < existing_version && !owner.empty() && owner != existing_subtask) {
        (void)co_await service::operation::settle_subtask(transaction, tenant_id, owner,
            service::operation::state_merged, existing_subtask, closed_by);
        origin.touch(existing_operation);
    }
    co_return std::string(rows.front()[0].value().value_or(""));
}

// Removes a resource's marker (the resource is deleted or needs no more
// work): its subtask is cancelled, or completed when its work already ran.
inline ruvia::task<void> remove_marker(ruvia::db_transaction& transaction, std::string_view tenant_id,
    marker_resource_type resource_type,
    std::string_view resource_id, service::operation::work_origin& origin) {
    const auto column = resource_column(resource_type);
    const auto removed = co_await transaction.query(
        "DELETE FROM sys_sync_task WHERE tenant_id = $1 AND resource_type = $2 AND " + std::string(column) +
            " = $3 RETURNING COALESCE(subtask_id::text, ''), is_done AND is_ok",
        tenant_id, resource_type_name(resource_type), resource_id);
    if (removed.empty()) {
        co_return;
    }
    const auto subtask = std::string(removed.front()[0].value().value_or(""));
    if (subtask.empty()) {
        co_return;
    }
    std::optional<std::string> settled;
    if (removed.front()[1].as<bool>().value_or(false)) {
        settled = co_await service::operation::settle_subtask(transaction, tenant_id, subtask,
            service::operation::state_completed);
    } else {
        // A user operation owns its cancellation even when it schedules
        // no background work of its own.
        settled = co_await service::operation::settle_subtask(transaction, tenant_id, subtask,
            service::operation::state_cancelled, {}, origin.operation_id().value_or(""));
    }
    if (settled) {
        origin.touch(*settled);
    }
}

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
        for (const auto& notification : notifications) co_await context.record_committed_notification(notification);
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

// Notifies the tasks a committed background transaction changed (nothing
// when it changed none).
template <typename context_type>
inline ruvia::task<void> commit_origin(ruvia::db_transaction& transaction, context_type& context, std::string_view tenant_id,
    const service::operation::work_origin& origin) {
    service::live_resource::notifications::resource_notification notification(tenant_id);
    origin.add_changes(notification);
    co_await service::live_resource::commit_and_notify(transaction, context, notification);
}

template <typename context_type>
inline ruvia::task<void> recover_stale_running(context_type& context, marker_resource_type resource_type) {
    auto transaction = co_await context.db().begin_transaction();
    const auto rows = co_await transaction.query(
        "UPDATE sys_sync_task task SET is_done = FALSE, is_ok = FALSE, count_fails = count_fails + 1, "
        "error = $2, next_attempt_at = NOW() + CAST($3 AS BIGINT) * INTERVAL '1 second', "
        "lease_owner = NULL, lease_until = NULL, updated_at = NOW() WHERE resource_type = $1 "
        "AND lease_until IS NOT NULL AND lease_until <= NOW() RETURNING tenant_id, resource_id, " +
            claimed_owner_columns("task"),
        resource_type_name(resource_type), std::string_view{lease_recovery_retry_error},
        retry_delay_seconds);
    std::vector<service::live_resource::notifications::resource_notification> notifications;
    for (const auto& row : rows) {
        auto& notification = tenant_notification(notifications, row[0].value().value_or(""));
        notification.changed(detail::resource_for_marker_type(resource_type), row[1].value().value_or(""));
        const auto owner = claimed_owner(row, 2);
        if (!owner.operation_id_.empty()) {
            notification.changed(service::live_resource::resource::operations, owner.operation_id_);
        }
    }
    co_await commit_tenant_changes(transaction, context, std::move(notifications));
}

template <typename database_type>
inline ruvia::task<bool> renew_running_lease(database_type& database, const running_marker_lease& lease) {
    const auto result = co_await database.execute(
        "UPDATE sys_sync_task SET lease_until = NOW() + INTERVAL '60 seconds' WHERE "
        "tenant_id = $1 AND id = $2 AND version = $3 AND lease_owner = $4 AND "
        "lease_until IS NOT NULL",
        lease.marker_.tenant_id_, lease.marker_.marker_id_, lease.marker_.version_, lease.owner_);
    co_return result.affected_rows() != 0;
}

template <typename database_type>
inline ruvia::task<bool> complete_running(database_type& database, const running_marker_lease& lease) {
    const auto result = co_await database.execute(
        "UPDATE sys_sync_task SET is_done = CASE WHEN version = $3 THEN TRUE ELSE FALSE END, "
        "is_ok = CASE WHEN version = $3 THEN TRUE ELSE FALSE END, "
        "processed_version = CASE WHEN version = $3 THEN $3 ELSE processed_version END, "
        "error = CASE WHEN version = $3 THEN '' ELSE error END, count_fails = CASE WHEN version = "
        "$3 THEN 0 ELSE count_fails END, lease_owner = NULL, lease_until = NULL, "
        "next_attempt_at = NOW(), updated_at = NOW() WHERE tenant_id = $1 AND id = $2 AND "
        "lease_owner = $4 AND lease_until IS NOT NULL",
        lease.marker_.tenant_id_, lease.marker_.marker_id_, lease.marker_.version_, lease.owner_);
    co_return result.affected_rows() != 0;
}

// The attempt row a result recorded and the subtask it belongs to.
struct recorded_attempt final {
    std::int64_t event_id_{};
    service::operation::work_owner owner_{};
};

// Records the attempt that just transitioned the marker. Must run in the same
// transaction right after complete_running/fail_running: the row is copied
// from the marker (including the subtask doing its work) only while the
// marker is in the matching state, and that UPDATE holds the marker row lock
// that serializes attempt numbering. `detail` is validated before binding
// (accepted_detail), so an unusable detail is replaced by the dropped marker
// instead of failing the result transaction.
template <typename database_type>
inline ruvia::task<std::optional<recorded_attempt>> record_running_result_event(database_type& database,
    const running_marker_lease& lease,
    std::string_view outcome, std::string_view detail) {
    if (outcome != event_outcome_completed && outcome != event_outcome_failed) {
        throw std::invalid_argument("invalid sync marker event outcome");
    }
    const bool failed = outcome == event_outcome_failed;
    const auto elapsed = elapsed_milliseconds(lease.started_);
    ruvia::db_query source;
    const auto marker = [&](std::string_view column) { return source.column(column, "marker"); };
    const auto equals = [&](ruvia::db_expression left, ruvia::db_expression right) {
        return source.binary(left, ruvia::db_binary_operator::equal, right);
    };
    source.from("sys_sync_task", "marker");
    const auto emitted_at = join_attempt_clock(source);
    source.select({marker("tenant_id"), marker("id"), marker("resource_type"), marker("resource_id"),
        marker("operation"), marker("version"), source.cast(source.value(outcome), ruvia::db_data_type::text),
        source.call("left", {marker("error"), source.value(std::int64_t{1000})}),
        next_attempt_number(source, {.tenant_id_ = {"tenant_id", "marker"}, .task_id_ = {"id", "marker"}, .resource_id_ = {"resource_id", "marker"}, .version_ = {"version", "marker"}}),
        attempt_started_at(source, emitted_at, elapsed),
        source.cast(source.value(elapsed.value_or(0)), ruvia::db_data_type::big_int),
        failed ? marker("next_attempt_at") : source.cast(source.null_value(), ruvia::db_data_type::timestamp_tz),
        source.cast(source.value(accepted_detail(detail)), ruvia::db_data_type::jsonb), emitted_at,
        marker("subtask_id")});
    source.where(equals(marker("tenant_id"), source.value(lease.marker_.tenant_id_)))
        .and_where(equals(marker("id"), source.value(lease.marker_.marker_id_)))
        .and_where(equals(marker("version"), source.value(lease.marker_.version_)));
    if (failed) {
        source.and_where(source.unary(ruvia::db_unary_operator::not_value, marker("is_done")))
            .and_where(source.unary(ruvia::db_unary_operator::not_value, marker("is_ok")))
            .and_where(source.binary(marker("count_fails"), ruvia::db_binary_operator::greater,
                source.value(std::int64_t{0})));
    } else {
        source.and_where(marker("is_done")).and_where(marker("is_ok"));
    }
    ruvia::db_query insert;
    insert.insert_into("sys_sync_event", attempt_event_columns)
        .insert_from(source)
        .returning({insert.column("id"),
            insert.coalesce({insert.cast(insert.column("subtask_id"), ruvia::db_data_type::text),
                insert.value(std::string_view{})})});
    const auto rows = co_await database.query(insert);
    if (rows.empty()) {
        co_return std::nullopt;
    }
    recorded_attempt recorded{.event_id_ = rows.front()[0].template as<std::int64_t>().value_or(0),
        .owner_ = {.operation_id_ = {}, .subtask_id_ = std::string(rows.front()[1].value().value_or(""))}};
    if (!recorded.owner_.subtask_id_.empty()) {
        const auto owner = co_await database.query(
            "SELECT operation_id::text FROM sys_operation_subtask WHERE id = $1::uuid",
            std::string_view(recorded.owner_.subtask_id_));
        if (!owner.empty()) {
            recorded.owner_.operation_id_ = std::string(owner.front()[0].value().value_or(""));
        }
    }
    co_return recorded;
}

// Hourly retention: attempts older than 7 days (their traces cascade), then
// tasks created more than 7 days ago whose work is all settled and which
// recorded nothing within the period. Open histories of affected tasks resend
// once; lists re-read.
template <typename context_type>
inline ruvia::task<void> prune_result_events(context_type& context) {
    auto transaction = co_await context.db().begin_transaction();
    const auto removed = co_await transaction.query(
        "WITH removed AS (DELETE FROM sys_sync_event WHERE emitted_at < NOW() - INTERVAL '7 days' "
        "RETURNING subtask_id) SELECT DISTINCT subtask.tenant_id::text, subtask.operation_id::text FROM "
        "removed INNER JOIN sys_operation_subtask subtask ON subtask.id = removed.subtask_id");
    const auto expired = co_await transaction.query(
        "DELETE FROM sys_operation operation WHERE operation.created_at < NOW() - INTERVAL '7 days' AND "
        "NOT EXISTS (SELECT 1 FROM sys_operation_subtask subtask WHERE subtask.operation_id = operation.id "
        "AND subtask.state = 'open' AND (EXISTS (SELECT 1 FROM sys_sync_task marker WHERE "
        "marker.subtask_id = subtask.id AND NOT (marker.is_done AND marker.is_ok)) OR EXISTS (SELECT 1 FROM "
        "sys_node_release_target target WHERE target.subtask_id = subtask.id AND target.status IN "
        "('pending', 'failed')) OR (subtask.kind = 'node_config' AND EXISTS (SELECT 1 FROM sys_node node "
        "WHERE node.tenant_id = subtask.tenant_id AND node.id = subtask.resource_id AND node.deleted_at IS "
        "NULL AND node.node_spec_revision = subtask.version AND node.applied_node_spec_revision < "
        "subtask.version)))) AND NOT EXISTS (SELECT 1 FROM sys_operation_subtask subtask INNER JOIN "
        "sys_sync_event event ON event.subtask_id = subtask.id WHERE subtask.operation_id = operation.id "
        "AND event.emitted_at >= NOW() - INTERVAL '7 days') RETURNING operation.tenant_id::text, "
        "operation.id::text");
    std::vector<service::live_resource::notifications::resource_notification> notifications;
    for (const auto& row : removed) {
        tenant_notification(notifications, row[0].value().value_or(""))
            .task_history_reset(row[1].value().value_or(""));
    }
    for (const auto& row : expired) {
        tenant_notification(notifications, row[0].value().value_or(""))
            .changed(service::live_resource::resource::operations, row[1].value().value_or(""));
    }
    co_await commit_tenant_changes(transaction, context, std::move(notifications));
}

template <typename database_type>
inline ruvia::task<running_result_transition>
complete_running_and_record_event(database_type& database, const running_marker_lease& lease,
    std::string_view detail) {
    if (!co_await complete_running(database, lease)) {
        co_return running_result_transition{};
    }
    const auto recorded = co_await record_running_result_event(database, lease, event_outcome_completed, detail);
    co_return running_result_transition{
        .marker_transitioned_ = true,
        .event_recorded_ = recorded.has_value(),
        .event_id_ = recorded ? std::optional<std::int64_t>(recorded->event_id_) : std::nullopt,
        .owner_ = recorded ? recorded->owner_ : service::operation::work_owner{},
    };
}

template <typename database_type>
inline ruvia::task<bool> release_running(database_type& database, const running_marker_lease& lease) {
    const auto result = co_await database.execute(
        "UPDATE sys_sync_task SET lease_owner = NULL, lease_until = NULL, next_attempt_at = "
        "NOW(), updated_at = NOW() WHERE tenant_id = $1 AND id = $2 AND lease_owner = $3 AND "
        "lease_until IS NOT NULL",
        lease.marker_.tenant_id_, lease.marker_.marker_id_, lease.owner_);
    co_return result.affected_rows() != 0;
}

// Removes the marker whose work (a deletion) is done; its subtask completes.
// Returns the subtask's owner, or nothing when the lease was lost.
template <typename database_type>
inline ruvia::task<std::optional<service::operation::work_owner>> remove_running(database_type& database,
    const running_marker_lease& lease) {
    const auto removed = co_await database.query(
        "DELETE FROM sys_sync_task WHERE tenant_id = $1 AND id = $2 AND version = $3 AND "
        "lease_owner = $4 AND lease_until IS NOT NULL RETURNING COALESCE(subtask_id::text, '')",
        lease.marker_.tenant_id_, lease.marker_.marker_id_, lease.marker_.version_, lease.owner_);
    if (removed.empty()) {
        co_return std::nullopt;
    }
    service::operation::work_owner owner{.operation_id_ = {},
        .subtask_id_ = std::string(removed.front()[0].value().value_or(""))};
    if (!owner.subtask_id_.empty()) {
        const std::optional<std::string> settled = co_await service::operation::settle_subtask(database,
            lease.marker_.tenant_id_, std::string_view(owner.subtask_id_), service::operation::state_completed);
        owner.operation_id_ = settled.value_or("");
    }
    co_return owner;
}

template <typename database_type>
inline ruvia::task<bool> fail_running(database_type& database, const running_marker_lease& lease,
    std::string_view error) {
    const auto result = co_await database.execute(
        "UPDATE sys_sync_task SET is_done = FALSE, is_ok = FALSE, count_fails = count_fails + 1, "
        "error = $5, next_attempt_at = NOW() + CAST($6 AS BIGINT) * INTERVAL '1 second', "
        "lease_owner = NULL, lease_until = NULL, updated_at = NOW() WHERE tenant_id = $1 AND "
        "id = $2 AND version = $3 AND lease_owner = $4 AND lease_until IS NOT NULL",
        lease.marker_.tenant_id_, lease.marker_.marker_id_, lease.marker_.version_, lease.owner_, error,
        retry_delay_seconds);
    co_return result.affected_rows() != 0;
}

// A failure of work that belongs to no task (scheduled before the upgrade,
// by a server without task tracking, or re-queued by such a server after its
// subtask's version) becomes visible: the system task 「后台任务」 adopts the
// marker, so this and later attempts belong to it.
template <typename database_type>
inline ruvia::task<void> adopt_unowned_marker(database_type& database, const running_marker_lease& lease) {
    const auto rows = co_await database.query(
        "SELECT marker.resource_type, marker.resource_id::text, marker.operation FROM sys_sync_task marker "
        "WHERE marker.tenant_id = $1 AND marker.id = $2 AND NOT EXISTS (SELECT 1 FROM "
        "sys_operation_subtask owner WHERE owner.id = marker.subtask_id AND owner.version = marker.version)",
        lease.marker_.tenant_id_, lease.marker_.marker_id_);
    if (rows.empty()) {
        co_return;
    }
    const auto resource_type = std::string(rows.front()[0].value().value_or(""));
    const auto resource_id = std::string(rows.front()[1].value().value_or(""));
    const auto operation = require_marker_operation(rows.front()[2].value().value_or(""));
    const auto marker_type = resource_type == "provider"      ? marker_resource_type::provider
                             : resource_type == "dns_zone"    ? marker_resource_type::dns_zone
                             : resource_type == "certificate" ? marker_resource_type::certificate
                                                              : marker_resource_type::website;
    auto origin = service::operation::work_origin::system(service::operation::action::system_untracked,
        resource_type, resource_id);
    const auto subtask = co_await origin.require_subtask(database, lease.marker_.tenant_id_,
        marker_subtask_kind(marker_type, operation, service::operation::subtask_purpose::work), resource_type,
        resource_id, lease.marker_.version_);
    (void)co_await database.execute("UPDATE sys_sync_task SET subtask_id = $3::uuid WHERE tenant_id = $1 AND id = $2",
        lease.marker_.tenant_id_, lease.marker_.marker_id_, std::string_view(subtask.value_or("")));
}

template <typename database_type>
inline ruvia::task<running_result_transition>
fail_running_and_record_event(database_type& database, const running_marker_lease& lease,
    std::string_view error, std::string_view detail) {
    if (!co_await fail_running(database, lease, error)) {
        co_return running_result_transition{};
    }
    co_await adopt_unowned_marker(database, lease);
    const auto recorded = co_await record_running_result_event(database, lease, event_outcome_failed, detail);
    co_return running_result_transition{
        .marker_transitioned_ = true,
        .event_recorded_ = recorded.has_value(),
        .event_id_ = recorded ? std::optional<std::int64_t>(recorded->event_id_) : std::nullopt,
        .owner_ = recorded ? recorded->owner_ : service::operation::work_owner{},
    };
}

}  // namespace service::sync_runtime
