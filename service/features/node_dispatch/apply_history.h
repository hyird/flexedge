#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/db/db_query.h>
#include <ruvia/web/db/db_transaction.h>
#include <ruvia/web/model.h>

#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "service/features/operation/work_origin.h"
#include "service/features/sync_runtime/attempt_record.h"

namespace service::node_dispatch {

RUVIA_MODEL(node_apply_detail_dto, RUVIA_REQUIRED_FIELD(kind, ruvia::string),
    RUVIA_REQUIRED_FIELD(truncated, ruvia::bool_value), RUVIA_REQUIRED_FIELD(node_id, ruvia::string),
    RUVIA_REQUIRED_FIELD(source, ruvia::string), RUVIA_REQUIRED_FIELD(result, ruvia::string),
    RUVIA_REQUIRED_FIELD(phase, ruvia::string), RUVIA_REQUIRED_FIELD(error_code, ruvia::string),
    RUVIA_REQUIRED_FIELD(retryable, ruvia::bool_value));

// One node's application of a cluster release. `source_` is "apply_result"
// for the node's own report and "heartbeat" when a heartbeat confirms the
// release is active without a report. Manifest digests and node spec
// revisions are configuration versions and are never part of it.
struct node_apply_attempt final {
    std::string_view tenant_id_;
    std::string_view release_id_;
    std::string_view node_id_;
    std::string_view source_;
    bool applied_{};
    // "stage", "validate" or "activate" for a failure, otherwise empty.
    std::string_view phase_;
    std::string_view error_code_;
    std::string_view error_;
    bool retryable_{};
    // Time from sending the desired state on this control connection to the
    // report, measured monotonically; unknown when it was sent elsewhere.
    std::optional<std::int64_t> elapsed_milliseconds_;

    [[nodiscard]] std::string to_json(std::pmr::memory_resource* resource) const noexcept {
        return service::sync_runtime::serialize_detail(resource, [this](std::pmr::memory_resource* pool) {
            service::sync_runtime::detail_bounds bounds;
            node_apply_detail_dto output({.resource_ = pool});
            output.set<"kind">("node_apply");
            output.set<"node_id">(bounds.text(node_id_));
            output.set<"source">(bounds.text(source_));
            output.set<"result">(applied_ ? std::string_view{"applied"} : std::string_view{"failed"});
            output.set<"phase">(bounds.text(applied_ ? std::string_view{} : phase_));
            output.set<"error_code">(bounds.text(applied_ ? std::string_view{} : error_code_, 64));
            output.set<"retryable">(!applied_ && retryable_);
            output.set<"truncated">(bounds.truncated());
            return output;
        });
    }
};

// The recorded attempt row, its sequence (task id and release generation or
// node spec revision) and the subtask that owns it (empty for unowned work).
struct recorded_node_attempt final {
    std::int64_t event_id_{};
    std::int64_t version_{};
    service::operation::work_owner owner_{};
    std::string task_id_{};
};

namespace detail {

template <typename rows_type>
ruvia::task<std::optional<recorded_node_attempt>> recorded_attempt_row(ruvia::db_transaction& transaction,
    const rows_type& rows) {
    if (rows.empty()) {
        co_return std::nullopt;
    }
    recorded_node_attempt recorded{.event_id_ = rows.front()[0].template as<std::int64_t>().value_or(0),
        .version_ = rows.front()[1].template as<std::int64_t>().value_or(0),
        .owner_ = {.operation_id_ = {}, .subtask_id_ = std::string(rows.front()[2].value().value_or(""))},
        .task_id_ = std::string(rows.front()[3].value().value_or(""))};
    if (!recorded.owner_.subtask_id_.empty()) {
        const auto owner = co_await transaction.query(
            "SELECT operation_id::text FROM sys_operation_subtask WHERE id = $1::uuid",
            std::string_view(recorded.owner_.subtask_id_));
        if (!owner.empty()) {
            recorded.owner_.operation_id_ = std::string(owner.front()[0].value().value_or(""));
        }
    }
    co_return recorded;
}

}  // namespace detail

// A failed apply of work that belongs to no task becomes visible: the system
// task 「后台任务」 adopts the release target before the attempt is recorded.
inline ruvia::task<void> adopt_unowned_target(ruvia::db_transaction& transaction, std::string_view tenant_id,
    std::string_view release_id, std::string_view node_id) {
    const auto rows = co_await transaction.query(
        "SELECT release.generation FROM sys_node_release_target target INNER JOIN sys_cluster_release "
        "release ON release.tenant_id = target.tenant_id AND release.id = target.release_id WHERE "
        "target.tenant_id = $1 AND target.release_id = $2 AND target.node_id = $3 AND target.subtask_id IS NULL",
        tenant_id, release_id, node_id);
    if (rows.empty()) {
        co_return;
    }
    auto origin = service::operation::work_origin::system(service::operation::action::system_untracked, "node",
        std::string(node_id));
    const auto subtask = co_await origin.require_subtask(transaction, tenant_id,
        service::operation::subtask_kind::node_publish, "node", node_id,
        rows.front()[0].as<std::int64_t>().value_or(1));
    (void)co_await transaction.execute(
        "UPDATE sys_node_release_target SET subtask_id = $4::uuid WHERE tenant_id = $1 AND release_id = $2 AND "
        "node_id = $3",
        tenant_id, release_id, node_id, std::string_view(subtask.value_or("")));
}

// Records the attempt for the release target of (tenant, release, node) in
// the task history: task_id = release id, resource_id = node id, version =
// release generation, subtask = the target's subtask. Runs in the caller's
// transaction after the target row changed; the node row lock taken earlier
// in that transaction serializes the attempt numbering. The node name and
// agent software version are merged from sys_node at the time of the
// attempt. Returns the recorded row, if any.
inline ruvia::task<std::optional<recorded_node_attempt>> record_node_apply_attempt(ruvia::db_transaction& transaction,
    const node_apply_attempt& attempt, std::pmr::memory_resource* resource) {
    if (!attempt.applied_) {
        co_await adopt_unowned_target(transaction, attempt.tenant_id_, attempt.release_id_, attempt.node_id_);
    }
    const auto detail_json = attempt.to_json(resource);
    ruvia::db_query source;
    const auto target = [&](std::string_view column) { return source.column(column, "target"); };
    const auto equals = [&](ruvia::db_expression left, ruvia::db_expression right) {
        return source.binary(left, ruvia::db_binary_operator::equal, right);
    };
    const auto text = [&](std::string_view value) { return source.value(value); };
    source.from("sys_node_release_target", "target")
        .join(ruvia::db_join_type::inner, "sys_cluster_release",
            source.binary(equals(source.column("tenant_id", "release"), target("tenant_id")),
                ruvia::db_binary_operator::and_value, equals(source.column("id", "release"), target("release_id"))),
            "release")
        .join(ruvia::db_join_type::inner, "sys_node",
            source.binary(equals(source.column("tenant_id", "node"), target("tenant_id")),
                ruvia::db_binary_operator::and_value, equals(source.column("id", "node"), target("node_id"))),
            "node");
    const auto emitted_at = service::sync_runtime::join_attempt_clock(source);
    const auto agent_version = source.coalesce(
        {source.binary(source.column("runtime", "node"), ruvia::db_binary_operator::json_get_text,
             text("agent_version")),
            text("")});
    const auto node_fields = source.call("jsonb_build_object",
        {text("node_name"), source.call("left", {source.column("name", "node"), source.value(std::int64_t{256})}),
            text("agent_version"), source.call("left", {agent_version, source.value(std::int64_t{64})})});
    const auto stored_detail =
        source.binary(source.cast(source.value(service::sync_runtime::accepted_detail(detail_json)), ruvia::db_data_type::jsonb),
            ruvia::db_binary_operator::json_concat, node_fields);
    source.select({target("tenant_id"), target("release_id"),
        source.cast(text("node"), ruvia::db_data_type::text), target("node_id"),
        source.cast(text("publish"), ruvia::db_data_type::text), source.column("generation", "release"),
        source.cast(text(attempt.applied_ ? service::sync_runtime::event_outcome_completed
                                          : service::sync_runtime::event_outcome_failed),
            ruvia::db_data_type::text),
        source.call("left",
            {source.cast(text(attempt.applied_ ? std::string_view{} : attempt.error_), ruvia::db_data_type::text),
                source.value(std::int64_t{1000})}),
        service::sync_runtime::next_attempt_number(source, {.tenant_id_ = {"tenant_id", "target"}, .task_id_ = {"release_id", "target"}, .resource_id_ = {"node_id", "target"}, .version_ = {"generation", "release"}}),
        service::sync_runtime::attempt_started_at(source, emitted_at, attempt.elapsed_milliseconds_),
        source.cast(source.value(attempt.elapsed_milliseconds_.value_or(0)), ruvia::db_data_type::big_int),
        source.cast(source.null_value(), ruvia::db_data_type::timestamp_tz), stored_detail, emitted_at,
        target("subtask_id")});
    source.where(equals(target("tenant_id"), source.value(attempt.tenant_id_)))
        .and_where(equals(target("release_id"), source.value(attempt.release_id_)))
        .and_where(equals(target("node_id"), source.value(attempt.node_id_)));
    ruvia::db_query insert;
    insert.insert_into("sys_sync_event", service::sync_runtime::attempt_event_columns)
        .insert_from(source)
        .returning({insert.column("id"), insert.column("version"),
            insert.coalesce({insert.cast(insert.column("subtask_id"), ruvia::db_data_type::text),
                insert.value(std::string_view{})}),
            insert.cast(insert.column("task_id"), ruvia::db_data_type::text)});
    const auto rows = co_await transaction.query(insert);
    co_return co_await detail::recorded_attempt_row(transaction, rows);
}

// The node reported applying (or failing) node spec revision `revision`:
// open configuration subtasks of the node up to that revision (exactly that
// revision for a failure) record the attempt, and complete when applied.
// task_id = resource_id = node id, version = the subtask's spec revision.
inline ruvia::task<std::vector<recorded_node_attempt>> record_node_config_attempts(
    ruvia::db_transaction& transaction, const node_apply_attempt& attempt, std::int64_t revision,
    std::pmr::memory_resource* resource) {
    const auto subtasks = co_await transaction.query(
        "SELECT id::text FROM sys_operation_subtask WHERE tenant_id = $1 AND resource_id = $2::uuid AND kind = "
        "'node_config' AND state = 'open' AND (version = $3 OR ($4 AND version < $3)) ORDER BY version",
        attempt.tenant_id_, attempt.node_id_, revision, attempt.applied_);
    std::vector<recorded_node_attempt> recorded;
    const auto detail_json = attempt.to_json(resource);
    for (const auto& subtask_row : subtasks) {
        const auto subtask_id = std::string(subtask_row[0].value().value_or(""));
        const auto rows = co_await transaction.query(
            "INSERT INTO sys_sync_event (tenant_id, task_id, resource_type, resource_id, operation, version, "
            "outcome, error, attempt, started_at, duration_ms, next_attempt_at, detail, emitted_at, subtask_id) "
            "SELECT subtask.tenant_id, subtask.resource_id, 'node', subtask.resource_id, 'configure', "
            "subtask.version, $3, left($4, 1000), COALESCE((SELECT previous.attempt FROM sys_sync_event previous "
            "WHERE previous.tenant_id = subtask.tenant_id AND previous.task_id = subtask.resource_id AND "
            "previous.resource_id = subtask.resource_id AND previous.version = subtask.version ORDER BY "
            "previous.id DESC LIMIT 1), 0) + 1, CASE WHEN $5::bigint < 0 THEN NULL ELSE clock_timestamp() - "
            "$5::bigint * INTERVAL '1 millisecond' END, GREATEST($5::bigint, 0), NULL, $6::jsonb || "
            "jsonb_build_object('node_name', left(node.name, 256), 'agent_version', "
            "left(COALESCE(node.runtime->>'agent_version', ''), 64)), clock_timestamp(), subtask.id FROM "
            "sys_operation_subtask subtask INNER JOIN sys_node node ON node.tenant_id = subtask.tenant_id AND "
            "node.id = subtask.resource_id WHERE subtask.tenant_id = $1 AND subtask.id = $2::uuid RETURNING id, "
            "version, subtask_id::text, task_id::text",
            attempt.tenant_id_, std::string_view(subtask_id),
            attempt.applied_ ? service::sync_runtime::event_outcome_completed
                             : service::sync_runtime::event_outcome_failed,
            attempt.applied_ ? std::string_view{} : attempt.error_, attempt.elapsed_milliseconds_.value_or(-1),
            service::sync_runtime::accepted_detail(detail_json));
        if (auto row = co_await detail::recorded_attempt_row(transaction, rows)) {
            recorded.push_back(std::move(*row));
        }
        if (attempt.applied_) {
            (void)co_await service::operation::settle_subtask(transaction, attempt.tenant_id_, subtask_id,
                service::operation::state_completed);
        }
    }
    co_return recorded;
}

}  // namespace service::node_dispatch
