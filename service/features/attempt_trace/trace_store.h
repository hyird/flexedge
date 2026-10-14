#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/db/db_query.h>
#include <ruvia/web/db/db_types.h>

#include <cstdint>
#include <exception>
#include <string>
#include <string_view>

#include "service/features/attempt_trace/model.h"
#include "service/features/attempt_trace/recorder.h"
#include "service/features/logging/logger.h"

namespace service::attempt_trace {

inline constexpr std::string_view trace_table{"sys_sync_event_trace"};

// The attempts sharing one history: tenant, task, resource and version.
struct attempt_sequence final {
    std::string_view tenant_id_;
    std::string_view task_id_;
    std::string_view resource_id_;
    std::int64_t version_{};
};

// Stores the trace of the recorded attempt `event_id`, then drops full traces
// of the same sequence that are neither among the newest retained_traces
// attempts nor its first retained attempt. Their history rows stay.
template <typename database_type>
ruvia::task<void> insert_attempt_trace(database_type& database, const attempt_sequence& sequence,
    std::int64_t event_id, const stored_trace& trace) {
    ruvia::db_query insert;
    insert.insert_into(trace_table, {"event_id", "tenant_id", "summary", "trace", "trace_bytes"})
        .values({insert.value(event_id), insert.cast(insert.value(sequence.tenant_id_), ruvia::db_data_type::uuid),
            insert.cast(insert.value(std::string_view(trace.summary_json_)), ruvia::db_data_type::jsonb),
            insert.cast(insert.value(std::string_view(trace.trace_json_)), ruvia::db_data_type::jsonb),
            insert.cast(insert.value(trace.trace_bytes_), ruvia::db_data_type::integer)});
    (void)co_await database.execute(insert);

    ruvia::db_query ranked;
    const auto equals = [&](std::string_view column, ruvia::db_expression value) {
        return ranked.binary(ranked.column(column), ruvia::db_binary_operator::equal, value);
    };
    const auto position = [&](ruvia::db_order_direction direction) {
        return ranked.over(ranked.call("row_number"),
            {.order_by_ = {{.expression_ = ranked.column("id"), .direction_ = direction}}});
    };
    ranked
        .select({ranked.column("id"), ranked.alias(position(ruvia::db_order_direction::desc), "newest"),
            ranked.alias(position(ruvia::db_order_direction::asc), "oldest")})
        .from("sys_sync_event")
        .where(equals("tenant_id", ranked.cast(ranked.value(sequence.tenant_id_), ruvia::db_data_type::uuid)))
        .and_where(equals("task_id", ranked.cast(ranked.value(sequence.task_id_), ruvia::db_data_type::uuid)))
        .and_where(equals("resource_id", ranked.cast(ranked.value(sequence.resource_id_), ruvia::db_data_type::uuid)))
        .and_where(equals("version", ranked.value(sequence.version_)));
    ruvia::db_query prune;
    prune.delete_from(trace_table, "stored")
        .delete_using(ranked, "ranked")
        .where(prune.binary(prune.column("event_id", "stored"), ruvia::db_binary_operator::equal,
            prune.column("id", "ranked")))
        .and_where(prune.binary(prune.column("newest", "ranked"), ruvia::db_binary_operator::greater,
            prune.value(retained_traces)))
        .and_where(prune.binary(prune.column("oldest", "ranked"), ruvia::db_binary_operator::greater,
            prune.value(std::int64_t{1})));
    (void)co_await database.execute(prune);
    co_return;
}

// Finishes the recorder and stores its trace on the worker's autocommit
// handle after the attempt result committed. Never throws: a trace that
// cannot be stored leaves the attempt with `trace.state = none` and one log
// line naming the event and the SQLSTATE, never trace content.
template <typename database_type>
ruvia::task<void> store_attempt_trace(database_type& database, const attempt_sequence& sequence,
    std::int64_t event_id, recorder& trace) {
    std::string failure;
    try {
        const auto stored = trace.finish();
        if (!stored) {
            failure = "trace could not be serialized within its bounds";
        } else {
            co_await insert_attempt_trace(database, sequence, event_id, *stored);
        }
    } catch (const ruvia::db_error& error) {
        failure = "database error";
        if (const auto state = error.sql_state(); state) {
            failure.append(" SQLSTATE ").append(*state);
        }
        if (const auto constraint = error.constraint_name(); constraint) {
            failure.append(" constraint ").append(*constraint);
        }
    } catch (const std::exception&) {
        failure = "unexpected error";
    } catch (...) {
        failure = "unknown error";
    }
    if (!failure.empty()) {
        service::logging::error("Attempt trace not stored for task " + std::string(sequence.task_id_) + " event " +
                                std::to_string(event_id) + ": " + failure);
    }
    co_return;
}

}  // namespace service::attempt_trace
