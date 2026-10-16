#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/db/db_query.h>
#include <ruvia/web/db/db_transaction.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "service/features/attempt_trace/recorder.h"
#include "service/features/attempt_trace/trace_store.h"
#include "service/features/background/queued_work.h"
#include "service/features/background/work_available.h"
#include "service/features/live_resource/notification_outbox.h"
#include "service/features/sync_runtime/attempt_record.h"
#include "service/features/sync_runtime/work_commit.h"

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

// One row per marker type: everything the shared marker runtime knows about
// it. A marker is claimable while it is not done, due, unleased and not
// waiting, and its version is still the aggregate's `version_column_` of
// `table_` (soft-deleted aggregates only for `deleted_operation_`).
struct marker_type final {
    marker_resource_type resource_type_{};
    // sys_sync_task.resource_type and the column holding the aggregate id.
    std::string_view name_;
    std::string_view column_;
    // The claim loop (work_available type) that runs these markers.
    std::string_view work_type_;
    service::live_resource::resource resource_{};
    std::string_view table_;
    std::string_view version_column_;
    std::optional<marker_operation> deleted_operation_;
    // The aggregate's own view shows the claim (its sync state), not only its task.
    bool claim_changes_resource_{};
    // Markers one loop runs before it scans its maintenance again.
    std::size_t claims_per_round_{};
    std::string_view worker_label_;
    std::string_view unknown_failure_;
};

inline constexpr std::array marker_types{
    marker_type{.resource_type_ = marker_resource_type::provider,
        .name_ = "provider",
        .column_ = "provider_id",
        .work_type_ = service::background::work_available::provider_verification_work,
        .resource_ = service::live_resource::resource::providers,
        .table_ = "sys_provider",
        .version_column_ = "verification_generation",
        .deleted_operation_ = std::nullopt,
        .claim_changes_resource_ = false,
        .claims_per_round_ = 8,
        .worker_label_ = "Provider verification worker",
        .unknown_failure_ = "未知供应商检测错误"},
    marker_type{.resource_type_ = marker_resource_type::dns_zone,
        .name_ = "dns_zone",
        .column_ = "dns_zone_id",
        .work_type_ = service::background::work_available::dns_sync_work,
        .resource_ = service::live_resource::resource::dns_zones,
        .table_ = "sys_dns_zone",
        .version_column_ = "desired_revision",
        .deleted_operation_ = marker_operation::remove,
        .claim_changes_resource_ = true,
        .claims_per_round_ = 32,
        .worker_label_ = "DNS sync worker",
        .unknown_failure_ = "未知 DNS 同步错误"},
    marker_type{.resource_type_ = marker_resource_type::certificate,
        .name_ = "certificate",
        .column_ = "certificate_id",
        .work_type_ = service::background::work_available::certificate_work,
        .resource_ = service::live_resource::resource::certificates,
        .table_ = "sys_certificate",
        .version_column_ = "issuance_revision",
        .deleted_operation_ = std::nullopt,
        .claim_changes_resource_ = true,
        .claims_per_round_ = 4,
        .worker_label_ = "Certificate worker",
        .unknown_failure_ = "未知证书同步错误"},
    marker_type{.resource_type_ = marker_resource_type::website,
        .name_ = "website",
        .column_ = "website_id",
        .work_type_ = service::background::work_available::website_dispatch_work,
        .resource_ = service::live_resource::resource::websites,
        .table_ = "sys_website",
        .version_column_ = "revision",
        .deleted_operation_ = std::nullopt,
        .claim_changes_resource_ = false,
        .claims_per_round_ = 8,
        .worker_label_ = "Website sync worker",
        .unknown_failure_ = "网站同步发生未知错误"},
};

[[nodiscard]] inline constexpr const marker_type& describe(marker_resource_type resource_type) {
    for (const auto& type : marker_types) {
        if (type.resource_type_ == resource_type) {
            return type;
        }
    }
    throw std::invalid_argument("unsupported sync marker resource type");
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

// A marker parked on another task resumes when that task completes the
// awaited version or disappears.
struct marker_waiter final {
    marker_resource_type resource_type_{};
    std::string resource_id_;
};

// `event_id_` identifies the recorded attempt row when one was written.
// `resumed_`: the markers (of the same tenant) this result released.
struct running_result_transition final {
    bool marker_transitioned_{};
    bool event_recorded_{};
    std::optional<std::int64_t> event_id_;
    std::vector<marker_waiter> resumed_{};
};

inline constexpr std::int64_t retry_delay_seconds{15};
// The one marker lease length: a claim and every renewal hold the marker this
// long; an expired lease is recovered for retry.
inline constexpr std::chrono::seconds lease_duration{60};
// A resumed waiter runs this long after its awaited task completed, so the
// awaited result takes effect first (DNS propagation).
inline constexpr std::chrono::seconds wait_settle_delay{10};
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

[[nodiscard]] inline std::optional<marker_resource_type> parse_marker_resource_type(std::string_view name) {
    for (const auto& type : marker_types) {
        if (type.name_ == name) {
            return type.resource_type_;
        }
    }
    return std::nullopt;
}

namespace detail {

// The marker's task and its aggregate's view.
inline void marker_changed(service::live_resource::notifications::resource_notification& notification,
    marker_resource_type resource_type, std::string_view resource_id) {
    notification.changed(service::live_resource::resource::tasks, resource_id)
        .changed(describe(resource_type).resource_, resource_id);
}

// The committed result of one attempt: its task (with the recorded attempt
// when there is one), the marker's resource and the markers it resumed.
inline service::live_resource::notifications::resource_notification recorded_result_notification(
    const running_marker_lease& lease, const running_result_transition& transition) {
    service::live_resource::notifications::resource_notification notification(lease.marker_.tenant_id_);
    if (transition.event_id_) {
        notification.task_attempt(lease.marker_.resource_id_, *transition.event_id_);
    }
    marker_changed(notification, lease.marker_.resource_type_, lease.marker_.resource_id_);
    for (const auto& waiter : transition.resumed_) {
        marker_changed(notification, waiter.resource_type_, waiter.resource_id_);
    }
    return notification;
}

inline ruvia::db_expression equals(ruvia::db_query& query, ruvia::db_expression left, ruvia::db_expression right) {
    return query.binary(left, ruvia::db_binary_operator::equal, right);
}

// NOW() plus `duration`, bound as seconds.
inline ruvia::db_expression now_plus(ruvia::db_query& query, std::chrono::seconds duration) {
    const std::vector<ruvia::db_named_argument> interval{{"secs", query.value(std::int64_t{duration.count()})}};
    return query.binary(query.call("now"), ruvia::db_binary_operator::add, query.call("make_interval", {}, interval));
}

}  // namespace detail

// Commits the result, then stores the attempt trace with the worker's
// autocommit handle (a trace failure is logged and never undoes or delays the
// committed result), then notifies the change on the worker's own Redis
// connection (a notification failure is logged as well). `node_config`: the
// transaction may have changed node desired state (certificate material).
// `caused`: work the result queued; its tasks are notified and the
// committing worker wakes its own claim loops for it.
template <typename transaction_type, typename context_type>
inline ruvia::task<void> commit_and_publish_result_event(transaction_type& transaction, context_type& context,
    const running_marker_lease& lease, const running_result_transition& transition,
    service::attempt_trace::recorder& trace, bool node_config = false,
    const service::background::queued_work* caused = nullptr) {
    auto notification = transition.marker_transitioned_ || transition.event_recorded_
                            ? detail::recorded_result_notification(lease, transition)
                            : service::live_resource::notifications::resource_notification(lease.marker_.tenant_id_);
    if (node_config) {
        notification.node_config();
    }
    if (caused != nullptr) {
        add_changed_tasks(notification, *caused);
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
    if (caused != nullptr) {
        service::background::work_available::wake_own_claims(context, caused->claims());
    }
}

// Queues work on a resource at `version`: the resource's one task. The
// marker coalesces: a newer (or the same) version resets the task to queued
// work (and ends any wait), an older version leaves the current work
// unchanged. A running attempt of an older version loses its lease; one of
// the same version keeps it and its result counts for the reset task.
inline ruvia::task<std::string> upsert_marker(ruvia::db_transaction& transaction,
    std::string_view tenant_id,
    marker_resource_type resource_type,
    std::string_view resource_id, marker_operation operation,
    std::int64_t version, service::background::queued_work& work) {
    if (version <= 0) {
        throw std::invalid_argument("sync marker version must be positive");
    }
    if (!supports_marker_operation(resource_type, operation)) {
        throw std::invalid_argument("sync marker operation is not valid for this resource");
    }
    const auto& type = describe(resource_type);
    ruvia::db_query upsert;
    const auto current = [&](std::string_view column) { return upsert.column(column, "sys_sync_task"); };
    // `column` becomes `value` when the queued version is at least (`reset`)
    // or above (lease columns) the current one.
    const auto when_queued = [&](std::string_view column, ruvia::db_binary_operator comparison,
                                 ruvia::db_expression value) {
        return ruvia::db_assignment{.column_ = std::string(column),
            .value_ = upsert.case_when(
                {{.when_ = upsert.binary(upsert.excluded("version"), comparison, current("version")), .then_ = value}},
                current(column))};
    };
    const auto reset = [&](std::string_view column, ruvia::db_expression value) {
        return when_queued(column, ruvia::db_binary_operator::greater_equal, value);
    };
    const auto unlease = [&](std::string_view column) {
        return when_queued(column, ruvia::db_binary_operator::greater, upsert.null_value());
    };
    upsert
        .insert_into("sys_sync_task",
            {"tenant_id", "resource_type", type.column_, "operation", "version", "processed_version", "is_done",
                "is_ok", "error", "count_fails", "next_attempt_at", "created_at", "updated_at"})
        .values({upsert.value(tenant_id), upsert.value(type.name_), upsert.value(resource_id),
            upsert.value(marker_operation_name(operation)), upsert.value(version), upsert.value(std::int64_t{0}),
            upsert.value(false), upsert.value(false), upsert.value(std::string_view{}), upsert.value(std::int64_t{0}),
            upsert.call("now"), upsert.call("now"), upsert.call("now")})
        .on_conflict({.columns_ = {"tenant_id", "resource_type", "resource_id"},
            .update_ = {{.column_ = "version", .value_ = upsert.greatest({current("version"), upsert.excluded("version")})},
                reset("operation", upsert.excluded("operation")), reset("is_done", upsert.value(false)),
                reset("is_ok", upsert.value(false)), reset("processed_version", upsert.value(std::int64_t{0})),
                reset("error", upsert.value(std::string_view{})), reset("count_fails", upsert.value(std::int64_t{0})),
                reset("next_attempt_at", upsert.call("now")), reset("updated_at", upsert.call("now")),
                reset("wait_task_id", upsert.null_value()), reset("wait_version", upsert.null_value()),
                unlease("lease_owner"), unlease("lease_until")}})
        .returning({upsert.column("id")});
    const auto rows = co_await transaction.query(upsert);
    if (rows.empty()) {
        throw std::runtime_error("sync marker could not be created");
    }
    work.claim(type.work_type_);
    work.changed(resource_id);
    co_return std::string(rows.front()[0].value().value_or(""));
}

// Removes a resource's marker (the resource is deleted or needs no more
// work): its task disappears.
inline ruvia::task<void> remove_marker(ruvia::db_transaction& transaction, std::string_view tenant_id,
    marker_resource_type resource_type,
    std::string_view resource_id, service::background::queued_work& work) {
    const auto& type = describe(resource_type);
    ruvia::db_query removal;
    removal.delete_from("sys_sync_task")
        .where(detail::equals(removal, removal.column("tenant_id"), removal.value(tenant_id)))
        .and_where(detail::equals(removal, removal.column("resource_type"), removal.value(type.name_)))
        .and_where(detail::equals(removal, removal.column(type.column_), removal.value(resource_id)));
    const auto removed = co_await transaction.execute(removal);
    if (removed.affected_rows() != 0) {
        work.changed(resource_id);
    }
}

template <typename database_type>
inline ruvia::task<bool> renew_running_lease(database_type& database, const running_marker_lease& lease) {
    const auto result = co_await database.execute(
        "UPDATE sys_sync_task SET lease_until = NOW() + make_interval(secs => $5) WHERE "
        "tenant_id = $1 AND id = $2 AND version = $3 AND lease_owner = $4 AND "
        "lease_until IS NOT NULL",
        lease.marker_.tenant_id_, lease.marker_.marker_id_, lease.marker_.version_, lease.owner_,
        std::int64_t{lease_duration.count()});
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

// Releases the markers parked on the lease's marker; they run after
// wait_settle_delay. `removed`: the awaited marker is gone, so every waiter
// resumes; otherwise those awaiting at most the lease's version. Runs in the
// transaction that completes or removes the awaited marker.
template <typename database_type>
inline ruvia::task<std::vector<marker_waiter>> resume_waiters(database_type& database,
    const running_marker_lease& lease, bool removed) {
    ruvia::db_query resume;
    resume.update("sys_sync_task")
        .set("wait_task_id", resume.null_value())
        .set("wait_version", resume.null_value())
        .set("next_attempt_at", detail::now_plus(resume, wait_settle_delay))
        .set("updated_at", resume.call("now"))
        .where(detail::equals(resume, resume.column("tenant_id"), resume.value(lease.marker_.tenant_id_)))
        .and_where(detail::equals(resume, resume.column("wait_task_id"), resume.value(lease.marker_.marker_id_)));
    if (!removed) {
        resume.and_where(resume.binary(resume.column("wait_version"), ruvia::db_binary_operator::less_equal,
            resume.value(lease.marker_.version_)));
    }
    resume.returning({resume.column("resource_type"), resume.column("resource_id")});
    const auto rows = co_await database.query(resume);
    std::vector<marker_waiter> waiters;
    waiters.reserve(rows.size());
    for (const auto& row : rows) {
        const auto type = parse_marker_resource_type(row[0].value().value_or(""));
        if (!type) {
            throw std::runtime_error("invalid sync marker resource type");
        }
        waiters.push_back({.resource_type_ = *type, .resource_id_ = std::string(row[1].value().value_or(""))});
    }
    co_return waiters;
}

// Records the attempt that just transitioned the marker and returns its row
// id. Must run in the same transaction right after complete_running/
// fail_running/the park: the row is copied from the marker only while the
// marker is in the matching state, and that UPDATE holds the marker row lock
// that serializes attempt numbering. `detail` is validated before binding
// (accepted_detail), so an unusable detail is replaced by the dropped marker
// instead of failing the result transaction.
template <typename database_type>
inline ruvia::task<std::optional<std::int64_t>> record_running_result_event(database_type& database,
    const running_marker_lease& lease,
    std::string_view outcome, std::string_view detail) {
    if (outcome != event_outcome_completed && outcome != event_outcome_failed && outcome != event_outcome_progressed) {
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
        source.cast(source.value(accepted_detail(detail)), ruvia::db_data_type::jsonb), emitted_at});
    source.where(equals(marker("tenant_id"), source.value(lease.marker_.tenant_id_)))
        .and_where(equals(marker("id"), source.value(lease.marker_.marker_id_)))
        .and_where(equals(marker("version"), source.value(lease.marker_.version_)));
    if (failed) {
        source.and_where(source.unary(ruvia::db_unary_operator::not_value, marker("is_done")))
            .and_where(source.unary(ruvia::db_unary_operator::not_value, marker("is_ok")))
            .and_where(source.binary(marker("count_fails"), ruvia::db_binary_operator::greater,
                source.value(std::int64_t{0})));
    } else if (outcome == event_outcome_progressed) {
        source.and_where(source.unary(ruvia::db_unary_operator::not_value, marker("is_done")))
            .and_where(source.unary(ruvia::db_unary_operator::is_not_null, marker("wait_task_id")));
    } else {
        source.and_where(marker("is_done")).and_where(marker("is_ok"));
    }
    ruvia::db_query insert;
    insert.insert_into("sys_sync_event", attempt_event_columns)
        .insert_from(source)
        .returning({insert.column("id")});
    const auto rows = co_await database.query(insert);
    if (rows.empty()) {
        co_return std::nullopt;
    }
    co_return rows.front()[0].template as<std::int64_t>().value_or(0);
}

// Completes the attempt, records it and resumes the markers that awaited it.
template <typename database_type>
inline ruvia::task<running_result_transition>
complete_running_and_record_event(database_type& database, const running_marker_lease& lease,
    std::string_view detail) {
    if (!co_await complete_running(database, lease)) {
        co_return running_result_transition{};
    }
    const auto event_id = co_await record_running_result_event(database, lease, event_outcome_completed, detail);
    co_return running_result_transition{
        .marker_transitioned_ = true,
        .event_recorded_ = event_id.has_value(),
        .event_id_ = event_id,
        .resumed_ = co_await resume_waiters(database, lease, false),
    };
}

// Ends the attempt with its marker parked on another task (`wait_task_id` at
// `wait_version`): the progress is recorded and the marker is not claimable
// until resume_waiters (or the bounded wait scan) releases it.
template <typename database_type>
inline ruvia::task<running_result_transition> park_running_and_record_event(database_type& database,
    const running_marker_lease& lease, std::string_view wait_task_id, std::int64_t wait_version,
    std::string_view detail) {
    if (wait_task_id.empty() || wait_version <= 0) {
        throw std::invalid_argument("invalid sync marker wait");
    }
    const auto parked = co_await database.execute(
        "UPDATE sys_sync_task SET is_done = FALSE, is_ok = FALSE, count_fails = 0, error = '', "
        "wait_task_id = $5, wait_version = $6, lease_owner = NULL, lease_until = NULL, "
        "next_attempt_at = NOW(), updated_at = NOW() WHERE tenant_id = $1 AND id = $2 AND "
        "version = $3 AND lease_owner = $4 AND lease_until IS NOT NULL",
        lease.marker_.tenant_id_, lease.marker_.marker_id_, lease.marker_.version_, lease.owner_, wait_task_id,
        wait_version);
    if (parked.affected_rows() == 0) {
        co_return running_result_transition{};
    }
    const auto event_id = co_await record_running_result_event(database, lease, event_outcome_progressed, detail);
    co_return running_result_transition{
        .marker_transitioned_ = true,
        .event_recorded_ = event_id.has_value(),
        .event_id_ = event_id,
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

// Removes the marker whose work (a deletion) is done: the task disappears.
// False when the lease was lost. Its waiters resume with
// resume_waiters(…, true) in the same transaction.
template <typename database_type>
inline ruvia::task<bool> remove_running(database_type& database, const running_marker_lease& lease) {
    const auto removed = co_await database.execute(
        "DELETE FROM sys_sync_task WHERE tenant_id = $1 AND id = $2 AND version = $3 AND "
        "lease_owner = $4 AND lease_until IS NOT NULL",
        lease.marker_.tenant_id_, lease.marker_.marker_id_, lease.marker_.version_, lease.owner_);
    co_return removed.affected_rows() != 0;
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

template <typename database_type>
inline ruvia::task<running_result_transition>
fail_running_and_record_event(database_type& database, const running_marker_lease& lease,
    std::string_view error, std::string_view detail) {
    if (!co_await fail_running(database, lease, error)) {
        co_return running_result_transition{};
    }
    const auto event_id = co_await record_running_result_event(database, lease, event_outcome_failed, detail);
    co_return running_result_transition{
        .marker_transitioned_ = true,
        .event_recorded_ = event_id.has_value(),
        .event_id_ = event_id,
    };
}

}  // namespace service::sync_runtime
