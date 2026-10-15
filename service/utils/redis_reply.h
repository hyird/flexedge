#pragma once

#include <ruvia/web/redis/redis.h>

#include <stdexcept>
#include <string>
#include <string_view>

namespace service::utils {

inline void require_redis_success(const ruvia::redis_value& value, std::string_view operation) {
    if (value.kind() == ruvia::redis_value::kind_type::error) {
        throw std::runtime_error(std::string(operation) + ": " + std::string(value.error()));
    }
}

}  // namespace service::utils
