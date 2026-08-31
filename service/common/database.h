#pragma once

#include <concepts>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ruvia/web/db/DbTypes.h>

namespace service::common {

inline std::string escapeLikePattern(std::string_view input) {
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

// DbHandle consumes a span, so heterogeneous optional values need one short-lived owning
// parameter list at the query boundary.
template <typename... Values>
    requires(std::constructible_from<ruvia::DbValue, Values &&> && ...)
inline std::vector<ruvia::DbValue> dbParams(Values&&... values) {
    std::vector<ruvia::DbValue> params;
    params.reserve(sizeof...(Values));
    (params.emplace_back(std::forward<Values>(values)), ...);
    return params;
}

inline bool isUniqueConstraintViolation(const ruvia::DbError& error,
                                        std::string_view constraintName) {
    return error.sqlState() == "23505" && error.constraintName() == constraintName;
}

} // namespace service::common
