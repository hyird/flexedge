#pragma once

#include <ruvia/web/db/db_query.h>
#include <ruvia/web/db/db_types.h>

#include <concepts>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

// Renders a structured predicate built only from constants (no request
// values) as PostgreSQL text, so a raw query that is not yet structured can
// share the single definition used by structured queries.
template <typename build_type>
    requires std::invocable<build_type&, ruvia::db_query&>
inline std::string render_constant_predicate(build_type build) {
    ruvia::db_query query(std::pmr::new_delete_resource());
    const auto expression = build(query);
    const auto sql = ruvia::db_query::render_expression(expression, ruvia::db_driver::postgresql,
        std::pmr::new_delete_resource(),
        ruvia::db_parameter_mode::literal);
    return std::string(sql);
}

}  // namespace service::common
