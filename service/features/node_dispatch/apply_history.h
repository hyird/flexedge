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

// The recorded attempt row and the release generation it belongs to.
struct recorded_node_attempt final {
    std::int64_t event_id_{};
    std::int64_t version_{};
};

// Records the attempt for the release target of (tenant, release, node) in
// the task history: task_id = release id, resource_id = node id, version =
// release generation. Runs in the caller's transaction after the target row
// changed; the node row lock taken earlier in that transaction serializes the
// attempt numbering. The node name and agent software version are merged from
// sys_node at the time of the attempt. Returns the recorded row, if any.
inline ruvia::task<std::optional<recorded_node_attempt>> record_node_apply_attempt(ruvia::db_transaction& transaction,
    const node_apply_attempt& attempt, std::pmr::memory_resource* resource) {
    const auto detail = attempt.to_json(resource);
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
        source.binary(source.cast(source.value(service::sync_runtime::accepted_detail(detail)), ruvia::db_data_type::jsonb),
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
        source.cast(source.null_value(), ruvia::db_data_type::timestamp_tz), stored_detail, emitted_at});
    source.where(equals(target("tenant_id"), source.value(attempt.tenant_id_)))
        .and_where(equals(target("release_id"), source.value(attempt.release_id_)))
        .and_where(equals(target("node_id"), source.value(attempt.node_id_)));
    ruvia::db_query insert;
    insert.insert_into("sys_sync_event", service::sync_runtime::attempt_event_columns)
        .insert_from(source)
        .returning({insert.column("id"), insert.column("version")});
    const auto rows = co_await transaction.query(insert);
    if (rows.empty()) {
        co_return std::nullopt;
    }
    co_return recorded_node_attempt{.event_id_ = rows.front()[0].as<std::int64_t>().value_or(0),
        .version_ = rows.front()[1].as<std::int64_t>().value_or(0)};
}

}  // namespace service::node_dispatch
