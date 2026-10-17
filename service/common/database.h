#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/db/db_query.h>
#include <ruvia/web/db/db_rows.h>
#include <ruvia/web/db/db_types.h>

#include <concepts>
#include <cstdint>
#include <initializer_list>
#include <memory_resource>
#include <optional>
#include <string>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "service/common/http.h"

namespace service::common {

inline std::string escape_like_pattern(std::string_view input) {
    std::string output;
    output.reserve(input.size());
    for (const char character : input) {
        if (character == '\\' || character == '%' || character == '_') {
            output.push_back('\\');
        }
        output.push_back(character);
    }
    return output;
}

// db_handle consumes a span, so heterogeneous optional values need one short-lived owning
// parameter list at the query boundary.
template <typename... values_type>
    requires(std::constructible_from<ruvia::db_value, values_type &&> && ...)
inline std::vector<ruvia::db_value> db_params(values_type&&... values) {
    std::vector<ruvia::db_value> params;
    params.reserve(sizeof...(values_type));
    (params.emplace_back(std::forward<values_type>(values)), ...);
    return params;
}

inline bool is_unique_constraint_violation(const ruvia::db_error& error,
    std::string_view constraint_name) {
    return error.sql_state() == "23505" && error.constraint_name() == constraint_name;
}

// Trusted constant select-list items, one expression each; request values never
// belong here, they are bound through db_query::value().
inline std::vector<ruvia::db_expression> sql_columns(ruvia::db_query& query,
    std::initializer_list<std::string_view> columns) {
    std::vector<ruvia::db_expression> expressions;
    expressions.reserve(columns.size());
    for (const auto column : columns) {
        expressions.push_back(query.sql(column));
    }
    return expressions;
}

inline constexpr std::string_view iso8601_micros_sql_format{"'YYYY-MM-DD\"T\"HH24:MI:SS.USOF'"};

inline ruvia::db_expression db_all(ruvia::db_query& query,
    std::initializer_list<ruvia::db_expression> items) {
    auto it = items.begin();
    auto expression = *it++;
    for (; it != items.end(); ++it) {
        expression = query.binary(expression, ruvia::db_binary_operator::and_value, *it);
    }
    return expression;
}

inline ruvia::db_expression db_any(ruvia::db_query& query,
    std::initializer_list<ruvia::db_expression> items) {
    auto it = items.begin();
    auto expression = *it++;
    for (; it != items.end(); ++it) {
        expression = query.binary(expression, ruvia::db_binary_operator::or_value, *it);
    }
    return expression;
}

inline ruvia::db_expression db_in(ruvia::db_query& query, ruvia::db_expression value,
    std::initializer_list<std::string_view> items) {
    std::vector<ruvia::db_expression> values;
    values.reserve(items.size());
    for (const auto item : items) {
        values.push_back(query.value(item));
    }
    return query.binary(value, ruvia::db_binary_operator::in, query.list(values));
}

inline ruvia::db_expression db_json_text(ruvia::db_query& query, ruvia::db_expression value,
    std::string_view key) {
    return query.binary(value, ruvia::db_binary_operator::json_get_text, query.value(key));
}

struct page_rows final {
    std::int64_t total_{};
    ruvia::db_rows rows_;
};

// Total and one page of a filtered list. `filter` adds the FROM, joins and WHERE
// shared by the count and the page; `project` adds the page's select list and ORDER BY.
template <typename context_type, typename filter_type, typename project_type>
    requires std::invocable<filter_type&, ruvia::db_query&> &&
             std::invocable<project_type&, ruvia::db_query&>
ruvia::task<page_rows> query_page(context_type& c, filter_type filter, project_type project,
    std::int64_t page_size, std::int64_t skip) {
    ruvia::db_query count(c.pool());
    filter(count);
    count.select(count.aggregate("count", {count.star()}));
    const auto count_rows = co_await c.db().query(count);
    const auto total = count_rows.empty()
                           ? std::int64_t{0}
                           : count_rows.front()[0].template as<std::int64_t>().value_or(0);
    ruvia::db_query page(c.pool());
    filter(page);
    project(page);
    page.limit(static_cast<std::uint64_t>(page_size)).offset(static_cast<std::uint64_t>(skip));
    auto rows = co_await c.db().query(page);
    co_return page_rows{total, std::move(rows)};
}

template <typename page_dto_type>
inline page_dto_type init_page_dto(std::pmr::memory_resource* resource, std::int64_t total,
    std::int64_t page, std::int64_t page_size) {
    page_dto_type data(ruvia::model_options{.resource_ = resource});
    data.template set<"total">(total);
    data.template set<"page">(page);
    data.template set<"page_size">(page_size);
    data.template set<"total_pages">(total == 0 ? 0 : (total + page_size - 1) / page_size);
    return data;
}

// The errors a revision-guarded write of one resource kind raises.
struct revision_errors final {
    const app_error_def& not_found_;
    const app_error_def& conflict_;
};

// Checks the revision of the live row the caller selected (FOR UPDATE when the
// write relies on it): no row (`std::nullopt`) is `not_found_`, another
// revision `conflict_`.
inline void require_current_revision(std::optional<std::int64_t> current_revision, std::int64_t expected_revision,
    const revision_errors& errors) {
    if (!current_revision) {
        throw_app_error(errors.not_found_);
    }
    if (*current_revision != expected_revision) {
        throw_app_error(errors.conflict_);
    }
}

// The same check over the rows of a `SELECT revision, ...`.
inline void require_current_revision(const ruvia::db_rows& current, std::int64_t expected_revision,
    const revision_errors& errors) {
    require_current_revision(current.empty() ? std::nullopt
                                             : std::optional<std::int64_t>{current.front()[0].as<std::int64_t>().value_or(0)},
        expected_revision, errors);
}

// Completes a conditional write `... WHERE id AND tenant_id AND revision =
// expected AND deleted_at IS NULL`: when it wrote nothing, tells a missing or
// deleted row of `table` (`not_found_`) from a stale revision (`conflict_`).
template <typename database_type>
ruvia::task<void> require_revision_written(database_type& database, bool written, std::string_view table,
    std::string_view tenant_id, std::string_view id, const revision_errors& errors) {
    if (written) {
        co_return;
    }
    ruvia::db_query query(std::pmr::new_delete_resource());
    const auto equal = [&](std::string_view column, std::string_view value) {
        return query.binary(query.column(column), ruvia::db_binary_operator::equal, query.value(value));
    };
    query.select(query.column("revision"))
        .from(table)
        .where(query.binary(query.binary(equal("id", id), ruvia::db_binary_operator::and_value,
                                equal("tenant_id", tenant_id)),
            ruvia::db_binary_operator::and_value,
            query.unary(ruvia::db_unary_operator::is_null, query.column("deleted_at"))))
        .limit(1);
    const auto rows = co_await database.query(query);
    throw_app_error(rows.empty() ? errors.not_found_ : errors.conflict_);
}

}  // namespace service::common
