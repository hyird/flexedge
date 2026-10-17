#pragma once
#include <ruvia/web/model.h>

namespace service::security_config {
RUVIA_MODEL(custom_rule_dto,
    RUVIA_REQUIRED_FIELD(id, ruvia::string), RUVIA_REQUIRED_FIELD(enabled, ruvia::bool_value),
    RUVIA_REQUIRED_FIELD(expression, ruvia::string), RUVIA_REQUIRED_FIELD(action, ruvia::string),
    RUVIA_OPTIONAL_FIELD(response_status, ruvia::int64),
    RUVIA_OPTIONAL_FIELD(response_content_type, ruvia::string), RUVIA_OPTIONAL_FIELD(response_body, ruvia::string),
    RUVIA_OPTIONAL_FIELD(name, ruvia::string), RUVIA_OPTIONAL_FIELD(expression_mode, ruvia::bool_value),
    RUVIA_OPTIONAL_FIELD(skip_stages, ruvia::array<ruvia::string>));
RUVIA_MODEL(cc_rule_dto,
    RUVIA_REQUIRED_FIELD(id, ruvia::string), RUVIA_REQUIRED_FIELD(enabled, ruvia::bool_value),
    RUVIA_REQUIRED_FIELD(expression, ruvia::string), RUVIA_REQUIRED_FIELD(action, ruvia::string),
    RUVIA_REQUIRED_FIELD(requests, ruvia::int64), RUVIA_REQUIRED_FIELD(window_seconds, ruvia::int64),
    RUVIA_REQUIRED_FIELD(mitigation_seconds, ruvia::int64), RUVIA_OPTIONAL_FIELD(name, ruvia::string),
    RUVIA_OPTIONAL_FIELD(expression_mode, ruvia::bool_value), RUVIA_OPTIONAL_FIELD(response_status, ruvia::int64),
    RUVIA_OPTIONAL_FIELD(response_content_type, ruvia::string), RUVIA_OPTIONAL_FIELD(response_body, ruvia::string));
RUVIA_MODEL(managed_dto,
    RUVIA_REQUIRED_FIELD(enabled, ruvia::bool_value), RUVIA_REQUIRED_FIELD(paranoia_level, ruvia::int64),
    RUVIA_REQUIRED_FIELD(inbound_threshold, ruvia::int64), RUVIA_REQUIRED_FIELD(body_limit_bytes, ruvia::int64),
    RUVIA_REQUIRED_FIELD(detection_only, ruvia::bool_value), RUVIA_REQUIRED_FIELD(excluded_rules, ruvia::array<ruvia::int64>));
RUVIA_MODEL(list_dto, RUVIA_REQUIRED_FIELD(name, ruvia::string), RUVIA_REQUIRED_FIELD(type, ruvia::string),
    RUVIA_REQUIRED_FIELD(entries, ruvia::array<ruvia::string>));
RUVIA_MODEL(policy_dto,
    RUVIA_REQUIRED_FIELD(custom_rules, ruvia::array<custom_rule_dto>),
    RUVIA_REQUIRED_FIELD(cc_rules, ruvia::array<cc_rule_dto>), RUVIA_REQUIRED_FIELD(managed, managed_dto),
    RUVIA_REQUIRED_FIELD(lists, ruvia::array<list_dto>));
}  // namespace service::security_config
