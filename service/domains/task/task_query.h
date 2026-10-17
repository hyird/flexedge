#pragma once

#include <ruvia/web/db/db_repository.h>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "service/common/database.h"
#include "service/domains/task/task_entity.h"
#include "service/features/attempt_trace/model.h"
#include "service/features/node_presence/node_connection.h"

namespace service::task {

struct list_filter final {
    std::string status_{};
    std::string type_{};
    std::string keyword_{};
};

// Builds the task read model with Ruvia's query AST. The database applies
// filtering, aggregation and pagination before mapping owned, named projections.
// The executor is the caller's handle or repeatable-read transaction.
template <typename executor_type>
class task_query final {
public:
    task_query(executor_type& database, std::string_view tenant, std::pmr::memory_resource* pool)
        : database_(database),
          expressions_(pool),
          tenant_(tenant) {}

    auto list(const list_filter& filter, std::int64_t size, std::int64_t skip) {
        auto query = tasks();
        apply_filter(query, filter);
        query.order_by(column("task", "updated_at"), ruvia::db_order_direction::desc)
            .add_order_by(column("task", "resource_type"))
            .add_order_by(column("task", "resource_id"))
            .take(static_cast<std::uint64_t>(size))
            .skip(static_cast<std::uint64_t>(skip));
        return query;
    }

    auto counts(const list_filter& filter) {
        auto matching = source<marker_entity>("matching");
        matching.from_cte("tasks").select({{"total", count()}});
        apply_filter(matching, filter, "matching");
        auto query = tasks();
        query.select({{"total", matching.subquery(expressions_)},
            {"active", filtered_count(one_of(column("task", "status"), {"queued", "running", "retrying"}))},
            {"failed", filtered_count(equal(column("task", "status"), value("failed")))}});
        return query;
    }

    auto detail(std::string_view type, std::string_view id) {
        auto query = tasks();
        query.where(all({equal(column("task", "resource_type"), value(type)),
            equal(column("task", "resource_id"), expressions_.cast(value(id), ruvia::db_data_type::uuid))}));
        return query;
    }

    auto history(std::string_view type, std::string_view id, std::int64_t limit) {
        auto query = attempts(type, id);
        query.order_by(column("event", "id"), ruvia::db_order_direction::desc)
            .take(static_cast<std::uint64_t>(limit));
        return query;
    }

    auto recorded_attempts(std::string_view type, std::string_view id, std::span<const std::int64_t> ids) {
        auto query = attempts(type, id);
        std::vector<expression> values;
        values.reserve(ids.size());
        for (const auto event_id : ids) {
            values.push_back(value(event_id));
        }
        query.and_where(expressions_.binary(column("event", "id"), ruvia::db_binary_operator::in,
                            expressions_.list(values)))
            .order_by(column("event", "id"));
        return query;
    }

    auto history_summary(std::string_view type, std::string_view id) {
        auto query = events(type, id);
        const auto failed = equal(column("event", "outcome"), value("failed"));
        const auto completed = equal(column("event", "outcome"), value("completed"));
        query.select({{"attempt_count", count()}, {"failure_count", filtered_count(failed)},
            {"first_failure_at", format_time(expressions_.filter(
                                     expressions_.aggregate("min", {column("event", "emitted_at")}), failed))},
            {"last_success_at", format_time(expressions_.filter(
                                    expressions_.aggregate("max", {column("event", "emitted_at")}), completed))}});
        return query;
    }

    auto attempt_trace(std::string_view type, std::string_view id, std::int64_t attempt_id) {
        auto query = events(type, id);
        query.template join<event_trace_entity>(ruvia::db_join_type::left, "stored",
                 equal(column("stored", "event_id"), column("event", "id")))
            .and_where(equal(column("event", "id"), value(attempt_id)))
            .select({{"attempt_id", text(column("event", "id"))},
                {"resource_type", column("event", "resource_type")},
                {"resource_id", text(column("event", "resource_id"))},
                {"kind", expressions_.coalesce({json_text(column("event", "detail"), "kind"), value("none")})},
                {"attempt", column("event", "attempt")}, {"outcome", column("event", "outcome")},
                {"error", column("event", "error")}, {"started_at", format_time(column("event", "started_at"))},
                {"emitted_at", format_time(column("event", "emitted_at"))},
                {"duration_ms", column("event", "duration_ms")},
                {"worker_type", json_text(column("stored", "trace"), "worker_type")},
                {"capture", text(json(column("stored", "trace"), "capture"))},
                {"steps", text(json(column("stored", "trace"), "steps"))}});
        return query;
    }

private:
    using expression = ruvia::db_expression;

    template <typename entity_type>
    auto source(std::string_view alias) {
        return database_.template get_repository<entity_type>().create_query_builder(alias);
    }

    expression column(std::string_view table, std::string_view name) {
        return expressions_.column(name, table);
    }
    template <typename value_type>
    expression value(value_type&& input) {
        return expressions_.value(std::forward<value_type>(input));
    }
    expression equal(expression left, expression right) {
        return expressions_.binary(left, ruvia::db_binary_operator::equal, right);
    }
    expression different(expression left, expression right) {
        return expressions_.binary(left, ruvia::db_binary_operator::not_equal, right);
    }
    expression all(std::initializer_list<expression> values) {
        return service::common::db_all(expressions_, values);
    }
    expression either(std::initializer_list<expression> values) {
        return service::common::db_any(expressions_, values);
    }
    expression one_of(expression input, std::initializer_list<std::string_view> options) {
        return service::common::db_in(expressions_, input, options);
    }
    expression text(expression input) {
        return expressions_.cast(input, ruvia::db_data_type::text);
    }
    expression json(expression input, std::string_view key) {
        return expressions_.binary(input, ruvia::db_binary_operator::json_get, value(key));
    }
    expression json_text(expression input, std::string_view key) {
        return service::common::db_json_text(expressions_, input, key);
    }
    expression format_time(expression input) {
        return expressions_.coalesce({expressions_.call("to_char",
                                          {input, value("YYYY-MM-DD\"T\"HH24:MI:SS.USOF")}),
            value("")});
    }
    expression count() {
        return expressions_.aggregate("count", {expressions_.star()});
    }
    expression filtered_count(expression condition) {
        return expressions_.filter(count(), condition);
    }
    expression null_time() {
        return expressions_.cast(expressions_.null_value(), ruvia::db_data_type::timestamp_tz);
    }

    expression resource_join(std::string_view type, std::string_view table) {
        return all({equal(column("marker", "resource_type"), value(type)),
            equal(column(table, "tenant_id"), column("marker", "tenant_id")),
            equal(column(table, "id"), column("marker", "resource_id"))});
    }

    auto event_statistics(std::string_view type_table, std::string_view type_column, expression resource_type) {
        auto query = source<event_entity>("event");
        query.where(all({equal(column("event", "tenant_id"), value(tenant_)),
                        equal(column("event", "resource_type"), resource_type),
                        equal(column("event", "resource_id"), column(type_table, type_column))}))
            .select({{"attempt_count", count()}, {"last_success_at", expressions_.filter(
                                                                         expressions_.aggregate("max", {column("event", "emitted_at")}),
                                                                         equal(column("event", "outcome"), value("completed")))}});
        return query;
    }

    auto marker_tasks() {
        const auto successful = all({column("marker", "is_done"), column("marker", "is_ok")});
        auto statistics = event_statistics("marker", "resource_id", column("marker", "resource_type"));
        const auto status = expressions_.case_when({{all({column("marker", "is_done"), expressions_.unary(ruvia::db_unary_operator::not_value,
                                                                                           column("marker", "is_ok"))}),
                                                        value("failed")},
                                                       {successful, value("completed")},
                                                       {expressions_.coalesce({expressions_.binary(column("marker", "lease_until"),
                                                                                   ruvia::db_binary_operator::greater, expressions_.call("now")),
                                                            value(false)}),
                                                           value("running")},
                                                       {expressions_.binary(column("marker", "count_fails"), ruvia::db_binary_operator::greater,
                                                            value(std::int64_t{0})),
                                                           value("retrying")}},
            value("queued"));
        auto query = source<marker_entity>("marker");
        query.template join<website_entity>(ruvia::db_join_type::left, "website", resource_join("website", "website"))
            .template join<zone_entity>(ruvia::db_join_type::left, "zone", resource_join("dns_zone", "zone"))
            .template join<certificate_entity>(ruvia::db_join_type::left, "certificate", resource_join("certificate", "certificate"))
            .template join<provider_entity>(ruvia::db_join_type::left, "provider", resource_join("provider", "provider"))
            .join(ruvia::db_join_type::cross, statistics, "attempts", {}, {.lateral_ = true})
            .where(all({equal(column("marker", "tenant_id"), value(tenant_)),
                expressions_.unary(ruvia::db_unary_operator::is_not_null, expressions_.coalesce({column("website", "id"), column("zone", "id"), column("certificate", "id"), column("provider", "id")}))}))
            .select({{"resource_type", column("marker", "resource_type")}, {"resource_id", column("marker", "resource_id")},
                {"resource_kind", expressions_.coalesce({column("provider", "kind"), value("")})},
                {"name", expressions_.coalesce({json_text(column("website", "config"), "name"), column("zone", "domain"),
                             column("certificate", "domain"), column("provider", "name"), column("provider", "provider"), value("")})},
                {"operation", column("marker", "operation")}, {"status", status},
                {"error", expressions_.case_when({{one_of(status, {"retrying", "failed"}), column("marker", "error")}}, value(""))},
                {"next_attempt_at", expressions_.case_when({{equal(status, value("retrying")), column("marker", "next_attempt_at")}}, null_time())},
                {"updated_at", column("marker", "updated_at")},
                {"last_success_at", column("attempts", "last_success_at")},
                {"attempt_count", column("attempts", "attempt_count")}, {"failure_count", column("marker", "count_fails")},
                {"node_offline", value(false)}});
        return query;
    }

    expression node_release_status_expr() {
        return expressions_.case_when(
            {{equal(column("target", "status"), value("pending")), value("queued")},
                {equal(column("target", "status"), value("failed")),
                    expressions_.case_when(
                        {{expressions_.unary(ruvia::db_unary_operator::is_false,
                              column("target", "retryable")),
                            value("failed")}},
                        value("retrying"))}},
            value("completed"));
    }

    expression node_spec_status_expr(expression spec_pending, expression current_spec) {
        return expressions_.case_when(
            {{expressions_.unary(ruvia::db_unary_operator::not_value, spec_pending),
                 value("completed")},
                {all({current_spec,
                     expressions_.binary(column("node", "config_task_failures"),
                         ruvia::db_binary_operator::greater, value(std::int64_t{0}))}),
                    expressions_.case_when(
                        {{expressions_.unary(ruvia::db_unary_operator::is_false,
                              column("node", "config_task_retryable")),
                            value("failed")}},
                        value("retrying"))}},
            value("queued"));
    }

    expression node_updated_at_expr(expression spec_pending, expression current_spec) {
        return expressions_.greatest({column("target", "updated_at"),
            expressions_.case_when({{current_spec, column("node", "config_task_updated_at")}},
                null_time()),
            expressions_.case_when(
                {{all({spec_pending,
                      expressions_.unary(ruvia::db_unary_operator::not_value, current_spec)}),
                    column("node", "updated_at")}},
                null_time()),
            expressions_.coalesce(
                {column("node", "registered_at"), column("node", "created_at")})});
    }

    auto node_tasks() {
        auto target = source<release_entity>("release");
        target.template join<release_target_entity>(ruvia::db_join_type::inner, "release_target",
                  all({equal(column("release_target", "tenant_id"), column("release", "tenant_id")),
                      equal(column("release_target", "release_id"), column("release", "id"))}))
            .where(all({equal(column("release", "tenant_id"), value(tenant_)),
                equal(column("release", "cluster_id"), column("node", "cluster_id")),
                equal(column("release_target", "node_id"), column("node", "id"))}))
            .select({{"release_id", column("release_target", "release_id")},
                {"status", column("release_target", "status")},
                {"retryable", column("release_target", "retryable")},
                {"last_error", column("release_target", "last_error")},
                {"count_fails", column("release_target", "count_fails")},
                {"updated_at", column("release_target", "updated_at")}})
            .order_by(column("release", "generation"), ruvia::db_order_direction::desc)
            .take(1);
        auto statistics = event_statistics("node", "id", value("node"));
        const auto spec_pending = expressions_.binary(column("node", "node_spec_revision"),
            ruvia::db_binary_operator::greater, column("node", "applied_node_spec_revision"));
        const auto current_spec =
            equal(column("node", "config_task_revision"), column("node", "node_spec_revision"));
        const auto release_status = node_release_status_expr();
        const auto spec_status = node_spec_status_expr(spec_pending, current_spec);
        const auto rank = [&](expression status) {
            return expressions_.case_when(
                {{equal(status, value("failed")), value(std::int64_t{3})},
                    {equal(status, value("retrying")), value(std::int64_t{2})},
                    {equal(status, value("queued")), value(std::int64_t{1})}},
                value(std::int64_t{0}));
        };
        // Pick one current operation, then derive all its fields together.
        // Retained history contributes totals only, never scheduling state.
        const auto use_spec = all({spec_pending,
            expressions_.binary(
                rank(spec_status), ruvia::db_binary_operator::greater_equal, rank(release_status))});
        const auto status = expressions_.case_when({{use_spec, spec_status}}, release_status);
        const std::array interval_args{
            ruvia::db_named_argument{"secs", value(service::node::online_window_seconds)}};
        const auto recent = expressions_.coalesce(
            {expressions_.binary(column("node", "last_heartbeat_at"),
                 ruvia::db_binary_operator::greater_equal,
                 expressions_.binary(expressions_.call("now"), ruvia::db_binary_operator::subtract,
                     expressions_.call("make_interval", {}, interval_args))),
                value(false)});
        auto query = source<node_entity>("node");
        query.join(ruvia::db_join_type::left, target, "target", value(true), {.lateral_ = true})
            .join(ruvia::db_join_type::cross, statistics, "attempts", {}, {.lateral_ = true})
            .where(all({equal(column("node", "tenant_id"), value(tenant_)),
                equal(column("node", "registration_status"), value("registered")),
                expressions_.unary(ruvia::db_unary_operator::is_null, column("node", "deleted_at"))}))
            .select({{"resource_type", value("node")}, {"resource_id", column("node", "id")},
                {"resource_kind", value("")}, {"name", column("node", "name")},
                {"operation", expressions_.case_when({{use_spec, value("configure")}}, value("publish"))},
                {"status", status},
                {"error",
                    expressions_.case_when(
                        {{one_of(status, {"failed", "retrying"}),
                            expressions_.case_when(
                                {{use_spec, column("node", "config_task_error")}},
                                expressions_.coalesce({column("target", "last_error"), value("")}))}},
                        value(""))},
                {"next_attempt_at", null_time()},
                {"updated_at", node_updated_at_expr(spec_pending, current_spec)},
                {"last_success_at", column("attempts", "last_success_at")},
                {"attempt_count", column("attempts", "attempt_count")},
                {"failure_count",
                    expressions_.case_when(
                        {{equal(status, value("completed")), value(std::int64_t{0})},
                            {use_spec,
                                expressions_.case_when(
                                    {{current_spec, column("node", "config_task_failures")}},
                                    value(std::int64_t{0}))}},
                        expressions_.coalesce(
                            {column("target", "count_fails"), value(std::int64_t{0})}))},
                {"node_offline",
                    all({expressions_.unary(ruvia::db_unary_operator::not_value, recent),
                        different(status, value("completed"))})}});
        return query;
    }

    auto tasks() {
        auto markers = marker_tasks();
        auto nodes = node_tasks();
        markers.combine(ruvia::db_set_operation::union_all, nodes);
        auto query = source<marker_entity>("task");
        query.with("tasks", markers).from_cte("tasks").select({{"resource_type", column("task", "resource_type")}, {"resource_id", text(column("task", "resource_id"))}, {"resource_kind", column("task", "resource_kind")}, {"name", column("task", "name")}, {"operation", column("task", "operation")}, {"status", column("task", "status")}, {"error", column("task", "error")}, {"next_attempt_at", format_time(column("task", "next_attempt_at"))}, {"updated_at", format_time(column("task", "updated_at"))}, {"last_success_at", format_time(column("task", "last_success_at"))}, {"attempt_count", column("task", "attempt_count")}, {"failure_count", column("task", "failure_count")}, {"node_offline", column("task", "node_offline")}});
        return query;
    }

    void apply_filter(auto& query, const list_filter& filter, std::string_view alias = "task") {
        if (!filter.status_.empty()) {
            const auto status = column(alias, "status");
            query.and_where(filter.status_ == "active"      ? one_of(status, {"queued", "running", "retrying"})
                            : filter.status_ == "attention" ? one_of(status, {"retrying", "failed"})
                                                            : equal(status, value(filter.status_)));
        }
        if (!filter.type_.empty()) {
            query.and_where(equal(column(alias, "resource_type"), value(filter.type_)));
        }
        if (!filter.keyword_.empty()) {
            const auto keyword = expressions_.call("lower", {value(filter.keyword_)});
            query.and_where(either({expressions_.binary(expressions_.call("strpos", {expressions_.call("lower", {column(alias, "name")}), keyword}), ruvia::db_binary_operator::greater, value(std::int64_t{0})),
                expressions_.binary(expressions_.call("strpos", {text(column(alias, "resource_id")), keyword}),
                    ruvia::db_binary_operator::greater, value(std::int64_t{0}))}));
        }
    }

    auto events(std::string_view type, std::string_view id) {
        auto query = source<event_entity>("event");
        query.where(all({equal(column("event", "tenant_id"), value(tenant_)),
            equal(column("event", "resource_type"), value(type)), equal(column("event", "resource_id"), value(id))}));
        return query;
    }

    auto attempts(std::string_view type, std::string_view id) {
        auto query = events(type, id);
        const auto summary = expressions_.binary(expressions_.coalesce({column("stored", "summary"),
                                                     expressions_.cast(value(service::attempt_trace::summary_none_json), ruvia::db_data_type::jsonb)}),
            ruvia::db_binary_operator::json_concat, expressions_.call("jsonb_build_object", {value("attempt_id"), text(column("event", "id"))}));
        query.template join<event_trace_entity>(ruvia::db_join_type::left, "stored",
                 equal(column("stored", "event_id"), column("event", "id")))
            .select({{"id", text(column("event", "id"))}, {"attempt", column("event", "attempt")},
                {"operation", column("event", "operation")}, {"outcome", column("event", "outcome")},
                {"error", column("event", "error")}, {"started_at", format_time(column("event", "started_at"))},
                {"emitted_at", format_time(column("event", "emitted_at"))}, {"duration_ms", column("event", "duration_ms")},
                {"detail", text(column("event", "detail"))}, {"trace", text(summary)}});
        return query;
    }

    executor_type& database_;
    ruvia::db_query expressions_;
    std::string tenant_{};
};

}  // namespace service::task
