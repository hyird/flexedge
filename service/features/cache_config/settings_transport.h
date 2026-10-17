#pragma once
#include <ruvia/web/model.h>
namespace service::cache_config {
RUVIA_MODEL(status_ttl_dto,
    RUVIA_REQUIRED_FIELD(from, ruvia::int64),
    RUVIA_REQUIRED_FIELD(to, ruvia::int64),
    RUVIA_REQUIRED_FIELD(seconds, ruvia::int64));
RUVIA_MODEL(header_key_dto,
    RUVIA_REQUIRED_FIELD(name, ruvia::string),
    RUVIA_REQUIRED_FIELD(values, ruvia::array<ruvia::string>));
RUVIA_MODEL(cache_key_dto,
    RUVIA_REQUIRED_FIELD(query_mode, ruvia::string),
    RUVIA_REQUIRED_FIELD(query_parameters, ruvia::array<ruvia::string>),
    RUVIA_REQUIRED_FIELD(headers, ruvia::array<ruvia::string>),
    RUVIA_REQUIRED_FIELD(header_presence, ruvia::array<ruvia::string>),
    RUVIA_REQUIRED_FIELD(header_values, ruvia::array<header_key_dto>),
    RUVIA_REQUIRED_FIELD(cookies, ruvia::array<ruvia::string>),
    RUVIA_REQUIRED_FIELD(cookie_presence, ruvia::array<ruvia::string>),
    RUVIA_REQUIRED_FIELD(include_origin, ruvia::bool_value),
    RUVIA_REQUIRED_FIELD(host, ruvia::string),
    RUVIA_REQUIRED_FIELD(country, ruvia::bool_value),
    RUVIA_REQUIRED_FIELD(language, ruvia::bool_value));
RUVIA_MODEL(vary_header_dto,
    RUVIA_REQUIRED_FIELD(name, ruvia::string),
    RUVIA_REQUIRED_FIELD(action, ruvia::string),
    RUVIA_REQUIRED_FIELD(values, ruvia::array<ruvia::string>));
RUVIA_MODEL(rule_settings_dto,
    RUVIA_REQUIRED_FIELD(enabled, ruvia::array<ruvia::string>),
    RUVIA_REQUIRED_FIELD(status_ttl, ruvia::array<status_ttl_dto>),
    RUVIA_REQUIRED_FIELD(key, cache_key_dto),
    RUVIA_REQUIRED_FIELD(armor, ruvia::bool_value),
    RUVIA_REQUIRED_FIELD(device, ruvia::bool_value),
    RUVIA_REQUIRED_FIELD(sort_query, ruvia::bool_value),
    RUVIA_REQUIRED_FIELD(serve_stale, ruvia::bool_value),
    RUVIA_REQUIRED_FIELD(strong_etags, ruvia::bool_value),
    RUVIA_REQUIRED_FIELD(origin_control, ruvia::bool_value),
    RUVIA_REQUIRED_FIELD(vary_default, ruvia::string),
    RUVIA_REQUIRED_FIELD(vary_headers, ruvia::array<vary_header_dto>),
    RUVIA_REQUIRED_FIELD(origin_range, ruvia::string),
    RUVIA_REQUIRED_FIELD(ports, ruvia::array<ruvia::int64>),
    RUVIA_REQUIRED_FIELD(read_timeout, ruvia::int64),
    RUVIA_REQUIRED_FIELD(error_passthrough, ruvia::bool_value));
}  // namespace service::cache_config
