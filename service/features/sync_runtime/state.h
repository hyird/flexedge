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
#include "service/features/live_resource/fanout.h"
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

// `event_id_` identifies the recorded attempt row when one was written.
struct running_result_transition final {
    bool marker_transitioned_{};
    bool event_recorded_{};
    std::optional<std::int64_t> event_id_;
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
    return resource::tasks;
}

inline void publish_recorded_result_event(const running_marker_lease& lease) {
    const auto resource_value = resource_for_marker_type(lease.marker_.resource_type_);
    using service::live_resource::resource;
    service::live_resource::get_hub().publish(lease.marker_.tenant_id_, resource::tasks,
        lease.marker_.marker_id_);
    service::live_resource::get_hub().publish(lease.marker_.tenant_id_, resource_value,
        lease.marker_.resource_id_);
}

}  // namespace detail

// Commits the result, then stores the attempt trace with the worker's
// autocommit handle (a trace failure is logged and never undoes or delays the
// committed result), then publishes the change.
template <typename transaction_type, typename database_type>
inline ruvia::task<void> commit_and_publish_result_event(transaction_type& transaction, database_type&& database,
    const running_marker_lease& lease, const running_result_transition& transition,
    service::attempt_trace::recorder& trace) {
    co_await transaction.commit();
    if (transition.event_id_) {
        co_await service::attempt_trace::store_attempt_trace(database,
            {.tenant_id_ = lease.marker_.tenant_id_,
                .task_id_ = lease.marker_.marker_id_,
                .resource_id_ = lease.marker_.resource_id_,
                .version_ = lease.marker_.version_},
            *transition.event_id_, trace);
    }
    if (transition.event_recorded_) {
        detail::publish_recorded_result_event(lease);
    }
    co_return;
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

inline ruvia::task<std::string> upsert_marker(ruvia::db_transaction& transaction,
    std::string_view tenant_id,
    marker_resource_type resource_type,
    std::string_view resource_id, marker_operation operation,
    std::int64_t version) {
    if (version <= 0) {
        throw std::invalid_argument("sync marker version must be positive");
    }
    if (!supports_marker_operation(resource_type, operation)) {
        throw std::invalid_argument("sync marker operation is not valid for this resource");
    }
    const auto column = resource_column(resource_type);
    const auto resource_type_value = resource_type_name(resource_type);
    const auto operation_value = marker_operation_name(operation);
    const auto rows = co_await transaction.query(
        "INSERT INTO sys_sync_task (tenant_id, resource_type, " + std::string(column) +
            ", operation, version, processed_version, is_done, is_ok, error, count_fails, "
            "next_attempt_at, created_at, updated_at) VALUES ($1, $2, $3, $4, $5, 0, FALSE, "
            "FALSE, '', 0, NOW(), NOW(), NOW()) ON CONFLICT (tenant_id, resource_type, "
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
            "sys_sync_task.lease_until END RETURNING id",
        tenant_id, resource_type_value, resource_id, operation_value, version);
    if (rows.empty()) {
        throw std::runtime_error("sync marker could not be created");
    }
    co_return std::string(rows.front()[0].value().value_or(""));
}

inline ruvia::task<void> remove_marker(ruvia::db_transaction& transaction, std::string_view tenant_id,
    marker_resource_type resource_type,
    std::string_view resource_id) {
    const auto column = resource_column(resource_type);
    (void)co_await transaction.execute(
        "DELETE FROM sys_sync_task WHERE tenant_id = $1 AND resource_type = $2 AND " +
            std::string(column) + " = $3",
        tenant_id, resource_type_name(resource_type), resource_id);
    co_return;
}

template <typename database_type>
inline ruvia::task<void> recover_stale_running(database_type& database, marker_resource_type resource_type) {
    const auto rows = co_await database.query(
        "UPDATE sys_sync_task SET is_done = FALSE, is_ok = FALSE, count_fails = count_fails + 1, "
        "error = $2, next_attempt_at = NOW() + CAST($3 AS BIGINT) * INTERVAL '1 second', "
        "lease_owner = NULL, lease_until = NULL, updated_at = NOW() WHERE resource_type = $1 "
        "AND lease_until IS NOT NULL AND lease_until <= NOW() RETURNING tenant_id, id, resource_id",
        resource_type_name(resource_type), std::string_view{lease_recovery_retry_error},
        retry_delay_seconds);
    for (const auto& row : rows) {
        service::live_resource::get_hub().publish(row[0].value().value_or(""),
            service::live_resource::resource::tasks,
            row[1].value().value_or(""));
        service::live_resource::get_hub().publish(
            row[0].value().value_or(""), detail::resource_for_marker_type(resource_type),
            row[2].value().value_or(""));
    }
    co_return;
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

// Records the attempt that just transitioned the marker. Must run in the same
// transaction right after complete_running/fail_running: the row is copied
// from the marker only while the marker is in the matching state, and that
// UPDATE holds the marker row lock that serializes attempt numbering. `detail`
// is validated before binding (accepted_detail), so an unusable detail is
// replaced by the dropped marker instead of failing the result transaction.
template <typename database_type>
inline ruvia::task<std::optional<std::int64_t>> record_running_result_event(database_type& database,
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
        source.cast(source.value(accepted_detail(detail)), ruvia::db_data_type::jsonb), emitted_at});
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
    insert.insert_into("sys_sync_event", attempt_event_columns).insert_from(source).returning({insert.column("id")});
    const auto rows = co_await database.query(insert);
    if (rows.empty()) {
        co_return std::nullopt;
    }
    co_return rows.front()[0].template as<std::int64_t>();
}

template <typename database_type>
inline ruvia::task<void> prune_result_events(database_type& database) {
    const auto removed = co_await database.query(
        "WITH removed AS (DELETE FROM sys_sync_event WHERE emitted_at < NOW() - INTERVAL '7 days' "
        "RETURNING tenant_id, task_id) SELECT DISTINCT tenant_id, task_id FROM removed");
    // The worker passes its autocommit database handle. Retention changes task
    // history and status projections just like any other committed mutation.
    for (const auto& row : removed) {
        service::live_resource::get_hub().publish(row[0].value().value_or(""),
            service::live_resource::resource::tasks,
            row[1].value().value_or(""));
    }
    co_return;
}

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

template <typename database_type>
inline ruvia::task<bool> remove_running(database_type& database, const running_marker_lease& lease) {
    const auto result = co_await database.execute(
        "DELETE FROM sys_sync_task WHERE tenant_id = $1 AND id = $2 AND version = $3 AND "
        "lease_owner = $4 AND lease_until IS NOT NULL",
        lease.marker_.tenant_id_, lease.marker_.marker_id_, lease.marker_.version_, lease.owner_);
    co_return result.affected_rows() != 0;
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
