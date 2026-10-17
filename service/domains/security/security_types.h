#pragma once
#include "service/features/security_config/transport.h"

namespace service::security {
RUVIA_MODEL(website_dto, RUVIA_REQUIRED_FIELD(id, ruvia::string), RUVIA_REQUIRED_FIELD(name, ruvia::string), RUVIA_REQUIRED_FIELD(policy_id, ruvia::string));
RUVIA_MODEL(websites_response, RUVIA_REQUIRED_FIELD(code, ruvia::int64), RUVIA_REQUIRED_FIELD(message, ruvia::string), RUVIA_REQUIRED_FIELD(data, ruvia::array<website_dto>));
RUVIA_MODEL(event_dto, RUVIA_REQUIRED_FIELD(id, ruvia::string), RUVIA_REQUIRED_FIELD(time, ruvia::string),
    RUVIA_REQUIRED_FIELD(ip, ruvia::string), RUVIA_REQUIRED_FIELD(country, ruvia::string), RUVIA_REQUIRED_FIELD(method, ruvia::string),
    RUVIA_REQUIRED_FIELD(path, ruvia::string), RUVIA_REQUIRED_FIELD(action, ruvia::string), RUVIA_REQUIRED_FIELD(stage, ruvia::string),
    RUVIA_REQUIRED_FIELD(ruleId, ruvia::string), RUVIA_REQUIRED_FIELD(ruleName, ruvia::string), RUVIA_REQUIRED_FIELD(policyId, ruvia::string),
    RUVIA_REQUIRED_FIELD(website, ruvia::string), RUVIA_REQUIRED_FIELD(detail, ruvia::string));
RUVIA_MODEL(hourly_dto, RUVIA_REQUIRED_FIELD(label, ruvia::string), RUVIA_REQUIRED_FIELD(block, ruvia::int64), RUVIA_REQUIRED_FIELD(challenge, ruvia::int64));
RUVIA_MODEL(distribution_dto, RUVIA_REQUIRED_FIELD(block, ruvia::int64), RUVIA_REQUIRED_FIELD(challenge, ruvia::int64), RUVIA_REQUIRED_FIELD(log, ruvia::int64), RUVIA_REQUIRED_FIELD(skip, ruvia::int64));
RUVIA_MODEL(telemetry_dto, RUVIA_REQUIRED_FIELD(requests, ruvia::int64), RUVIA_REQUIRED_FIELD(events, ruvia::array<event_dto>), RUVIA_REQUIRED_FIELD(hourly, ruvia::array<hourly_dto>), RUVIA_REQUIRED_FIELD(distribution, distribution_dto));
RUVIA_MODEL(telemetry_response, RUVIA_REQUIRED_FIELD(code, ruvia::int64), RUVIA_REQUIRED_FIELD(message, ruvia::string), RUVIA_REQUIRED_FIELD(data, telemetry_dto));
RUVIA_MODEL(request_sample, RUVIA_REQUIRED_FIELD(ip, ruvia::string), RUVIA_REQUIRED_FIELD(country, ruvia::string), RUVIA_REQUIRED_FIELD(asn, ruvia::string), RUVIA_REQUIRED_FIELD(method, ruvia::string), RUVIA_REQUIRED_FIELD(path, ruvia::string), RUVIA_REQUIRED_FIELD(host, ruvia::string), RUVIA_REQUIRED_FIELD(user_agent, ruvia::string));
RUVIA_MODEL(rule_test_input, RUVIA_REQUIRED_FIELD(request, request_sample), RUVIA_REQUIRED_FIELD(count, ruvia::int64), RUVIA_REQUIRED_FIELD(website_id, ruvia::string));
RUVIA_MODEL(rule_test_dto, RUVIA_REQUIRED_FIELD(matched, ruvia::bool_value), RUVIA_REQUIRED_FIELD(message, ruvia::string));
RUVIA_MODEL(rule_test_response, RUVIA_REQUIRED_FIELD(code, ruvia::int64), RUVIA_REQUIRED_FIELD(message, ruvia::string), RUVIA_REQUIRED_FIELD(data, rule_test_dto));
RUVIA_MODEL(list_member_dto, RUVIA_REQUIRED_FIELD(id, ruvia::string), RUVIA_REQUIRED_FIELD(value, ruvia::string),
    RUVIA_REQUIRED_FIELD(note, ruvia::string), RUVIA_REQUIRED_FIELD(source, ruvia::string), RUVIA_REQUIRED_FIELD(rule_id, ruvia::string),
    RUVIA_REQUIRED_FIELD(added_unix_ms, ruvia::int64), RUVIA_REQUIRED_FIELD(expires_unix_ms, ruvia::int64));
RUVIA_MODEL(list_input, RUVIA_REQUIRED_FIELD(name, ruvia::string), RUVIA_REQUIRED_FIELD(description, ruvia::string),
    RUVIA_REQUIRED_FIELD(kind, ruvia::string), RUVIA_REQUIRED_FIELD(members, ruvia::array<list_member_dto>));
RUVIA_MODEL(list_dto, RUVIA_REQUIRED_FIELD(id, ruvia::string), RUVIA_REQUIRED_FIELD(revision, ruvia::int64),
    RUVIA_REQUIRED_FIELD(name, ruvia::string), RUVIA_REQUIRED_FIELD(description, ruvia::string),
    RUVIA_REQUIRED_FIELD(kind, ruvia::string), RUVIA_REQUIRED_FIELD(members, ruvia::array<list_member_dto>));
RUVIA_MODEL(list_response, RUVIA_REQUIRED_FIELD(code, ruvia::int64), RUVIA_REQUIRED_FIELD(message, ruvia::string), RUVIA_REQUIRED_FIELD(data, list_dto));
RUVIA_MODEL(lists_response, RUVIA_REQUIRED_FIELD(code, ruvia::int64), RUVIA_REQUIRED_FIELD(message, ruvia::string), RUVIA_REQUIRED_FIELD(data, ruvia::array<list_dto>));
RUVIA_MODEL(managed_rule_dto, RUVIA_REQUIRED_FIELD(id, ruvia::int64),
    RUVIA_REQUIRED_FIELD(name, ruvia::string), RUVIA_REQUIRED_FIELD(category, ruvia::string),
    RUVIA_REQUIRED_FIELD(paranoia_level, ruvia::int64));
RUVIA_MODEL(managed_rules_response, RUVIA_REQUIRED_FIELD(code, ruvia::int64),
    RUVIA_REQUIRED_FIELD(message, ruvia::string), RUVIA_REQUIRED_FIELD(data, ruvia::array<managed_rule_dto>));
RUVIA_MODEL(policy_input, RUVIA_REQUIRED_FIELD(name, ruvia::string),
    RUVIA_REQUIRED_FIELD(description, ruvia::string), RUVIA_REQUIRED_FIELD(config, service::security_config::policy_dto));
RUVIA_MODEL(policy_dto, RUVIA_REQUIRED_FIELD(id, ruvia::string), RUVIA_REQUIRED_FIELD(name, ruvia::string),
    RUVIA_REQUIRED_FIELD(description, ruvia::string), RUVIA_REQUIRED_FIELD(revision, ruvia::int64),
    RUVIA_REQUIRED_FIELD(config, service::security_config::policy_dto), RUVIA_REQUIRED_FIELD(website_count, ruvia::int64),
    RUVIA_REQUIRED_FIELD(created_at, ruvia::string), RUVIA_REQUIRED_FIELD(updated_at, ruvia::string));
RUVIA_MODEL(policy_page, RUVIA_REQUIRED_FIELD(list, ruvia::array<policy_dto>),
    RUVIA_REQUIRED_FIELD(total, ruvia::int64), RUVIA_REQUIRED_FIELD(page, ruvia::int64),
    RUVIA_REQUIRED_FIELD(page_size, ruvia::int64), RUVIA_REQUIRED_FIELD(total_pages, ruvia::int64));
RUVIA_MODEL(policy_page_response, RUVIA_REQUIRED_FIELD(code, ruvia::int64), RUVIA_REQUIRED_FIELD(message, ruvia::string), RUVIA_REQUIRED_FIELD(data, policy_page));
RUVIA_MODEL(policy_response, RUVIA_REQUIRED_FIELD(code, ruvia::int64), RUVIA_REQUIRED_FIELD(message, ruvia::string), RUVIA_REQUIRED_FIELD(data, policy_dto));
RUVIA_MODEL(policy_option_dto, RUVIA_REQUIRED_FIELD(id, ruvia::string), RUVIA_REQUIRED_FIELD(name, ruvia::string),
    RUVIA_REQUIRED_FIELD(description, ruvia::string), RUVIA_REQUIRED_FIELD(website_count, ruvia::int64));
RUVIA_MODEL(policy_options_response, RUVIA_REQUIRED_FIELD(code, ruvia::int64), RUVIA_REQUIRED_FIELD(message, ruvia::string), RUVIA_REQUIRED_FIELD(data, ruvia::array<policy_option_dto>));
}  // namespace service::security
