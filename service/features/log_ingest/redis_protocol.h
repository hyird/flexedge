#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

#include <ruvia/web/redis/Redis.h>

namespace service::log_ingest {

inline void requireRedisSuccess(const ruvia::RedisValue& value, std::string_view operation) {
    if (value.kind() == ruvia::RedisValue::Kind::kError) {
        throw std::runtime_error(std::string(operation) + ": " + std::string(value.error()));
    }
}

} // namespace service::log_ingest
