#pragma once
#include <ruvia/web/model.h>
namespace service::cache_config {
RUVIA_MODEL(rule_dto,
    RUVIA_REQUIRED_FIELD(id, ruvia::string),
    RUVIA_REQUIRED_FIELD(name, ruvia::string),
    RUVIA_REQUIRED_FIELD(enabled, ruvia::bool_value),
    RUVIA_REQUIRED_FIELD(match_type, ruvia::string),
    RUVIA_REQUIRED_FIELD(patterns, ruvia::array<ruvia::string>),
    RUVIA_REQUIRED_FIELD(action, ruvia::string),
    RUVIA_REQUIRED_FIELD(ttl_mode, ruvia::string),
    RUVIA_REQUIRED_FIELD(ttl_seconds, ruvia::int64),
    RUVIA_REQUIRED_FIELD(ignore_query, ruvia::bool_value),
    RUVIA_REQUIRED_FIELD(methods, ruvia::array<ruvia::string>),
    RUVIA_REQUIRED_FIELD(status_codes, ruvia::array<ruvia::int64>),
    RUVIA_REQUIRED_FIELD(min_object_bytes, ruvia::int64),
    RUVIA_REQUIRED_FIELD(max_object_bytes, ruvia::int64),
    RUVIA_REQUIRED_FIELD(skip_set_cookie, ruvia::bool_value),
    RUVIA_REQUIRED_FIELD(browser_ttl_seconds, ruvia::int64),
    RUVIA_OPTIONAL_FIELD(skip_no_cache, ruvia::bool_value),
    RUVIA_OPTIONAL_FIELD(respect_request_no_cache, ruvia::bool_value));
RUVIA_MODEL(policy_dto,
    RUVIA_REQUIRED_FIELD(storage, ruvia::string),
    RUVIA_REQUIRED_FIELD(memory_bytes, ruvia::int64),
    RUVIA_REQUIRED_FIELD(disk_bytes, ruvia::int64),
    RUVIA_OPTIONAL_FIELD(memory_percent, ruvia::int64),
    RUVIA_OPTIONAL_FIELD(disk_percent, ruvia::int64),
    RUVIA_OPTIONAL_FIELD(memory_reserved_bytes, ruvia::int64),
    RUVIA_OPTIONAL_FIELD(disk_reserved_bytes, ruvia::int64),
    RUVIA_REQUIRED_FIELD(max_object_bytes, ruvia::int64),
    RUVIA_REQUIRED_FIELD(directory, ruvia::string),
    RUVIA_REQUIRED_FIELD(rules, ruvia::array<rule_dto>));

}  // namespace service::cache_config
