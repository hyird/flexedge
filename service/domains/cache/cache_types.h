#pragma once
#include <ruvia/web/model.h>

#include "service/features/cache_config/transport.h"
namespace service::cache {
RUVIA_MODEL(policy_input, RUVIA_REQUIRED_FIELD(name, ruvia::string),
    RUVIA_REQUIRED_FIELD(description, ruvia::string), RUVIA_REQUIRED_FIELD(status, ruvia::string),
    RUVIA_REQUIRED_FIELD(config, service::cache_config::policy_dto));
RUVIA_MODEL(policy_dto, RUVIA_REQUIRED_FIELD(id, ruvia::string), RUVIA_REQUIRED_FIELD(name, ruvia::string),
    RUVIA_REQUIRED_FIELD(description, ruvia::string), RUVIA_REQUIRED_FIELD(status, ruvia::string),
    RUVIA_REQUIRED_FIELD(revision, ruvia::int64), RUVIA_REQUIRED_FIELD(config, service::cache_config::policy_dto),
    RUVIA_REQUIRED_FIELD(website_count, ruvia::int64), RUVIA_REQUIRED_FIELD(created_at, ruvia::string),
    RUVIA_REQUIRED_FIELD(updated_at, ruvia::string));
RUVIA_MODEL(policy_page, RUVIA_REQUIRED_FIELD(list, ruvia::array<policy_dto>),
    RUVIA_REQUIRED_FIELD(total, ruvia::int64), RUVIA_REQUIRED_FIELD(page, ruvia::int64),
    RUVIA_REQUIRED_FIELD(page_size, ruvia::int64), RUVIA_REQUIRED_FIELD(total_pages, ruvia::int64));
RUVIA_MODEL(policy_page_response, RUVIA_REQUIRED_FIELD(code, ruvia::int64), RUVIA_REQUIRED_FIELD(message, ruvia::string), RUVIA_REQUIRED_FIELD(data, policy_page));
RUVIA_MODEL(policy_response, RUVIA_REQUIRED_FIELD(code, ruvia::int64), RUVIA_REQUIRED_FIELD(message, ruvia::string), RUVIA_REQUIRED_FIELD(data, policy_dto));
RUVIA_MODEL(policy_options_response, RUVIA_REQUIRED_FIELD(code, ruvia::int64), RUVIA_REQUIRED_FIELD(message, ruvia::string), RUVIA_REQUIRED_FIELD(data, ruvia::array<policy_dto>));
RUVIA_MODEL(website_option_dto, RUVIA_REQUIRED_FIELD(id, ruvia::string), RUVIA_REQUIRED_FIELD(name, ruvia::string),
    RUVIA_REQUIRED_FIELD(status, ruvia::string), RUVIA_REQUIRED_FIELD(cache_enabled, ruvia::bool_value),
    RUVIA_REQUIRED_FIELD(cache_policy_id, ruvia::string), RUVIA_REQUIRED_FIELD(domains, ruvia::array<ruvia::string>));
RUVIA_MODEL(website_options_response, RUVIA_REQUIRED_FIELD(code, ruvia::int64), RUVIA_REQUIRED_FIELD(message, ruvia::string), RUVIA_REQUIRED_FIELD(data, ruvia::array<website_option_dto>));
RUVIA_MODEL(operation_input, RUVIA_REQUIRED_FIELD(operation, ruvia::string), RUVIA_REQUIRED_FIELD(object_id, ruvia::string),
    RUVIA_REQUIRED_FIELD(scope, ruvia::string), RUVIA_REQUIRED_FIELD(urls, ruvia::array<ruvia::string>));
RUVIA_MODEL(operation_created, RUVIA_REQUIRED_FIELD(id, ruvia::string), RUVIA_REQUIRED_FIELD(target_node_count, ruvia::int64));
RUVIA_MODEL(operation_response, RUVIA_REQUIRED_FIELD(code, ruvia::int64), RUVIA_REQUIRED_FIELD(message, ruvia::string), RUVIA_REQUIRED_FIELD(data, operation_created));
RUVIA_MODEL(target_dto, RUVIA_REQUIRED_FIELD(node_id, ruvia::string), RUVIA_REQUIRED_FIELD(node_name, ruvia::string),
    RUVIA_REQUIRED_FIELD(status, ruvia::string), RUVIA_REQUIRED_FIELD(error, ruvia::string),
    RUVIA_REQUIRED_FIELD(affected_objects, ruvia::int64), RUVIA_REQUIRED_FIELD(affected_bytes, ruvia::int64),
    RUVIA_REQUIRED_FIELD(completed_urls, ruvia::int64), RUVIA_REQUIRED_FIELD(failures, ruvia::array<ruvia::string>));
RUVIA_MODEL(operation_dto, RUVIA_REQUIRED_FIELD(id, ruvia::string), RUVIA_REQUIRED_FIELD(operation, ruvia::string),
    RUVIA_REQUIRED_FIELD(object_id, ruvia::string), RUVIA_REQUIRED_FIELD(name, ruvia::string), RUVIA_REQUIRED_FIELD(scope, ruvia::string),
    RUVIA_REQUIRED_FIELD(status, ruvia::string), RUVIA_REQUIRED_FIELD(error, ruvia::string), RUVIA_REQUIRED_FIELD(updated_at, ruvia::string),
    RUVIA_REQUIRED_FIELD(targets, ruvia::array<target_dto>));
RUVIA_MODEL(operations_response, RUVIA_REQUIRED_FIELD(code, ruvia::int64), RUVIA_REQUIRED_FIELD(message, ruvia::string), RUVIA_REQUIRED_FIELD(data, ruvia::array<operation_dto>));
}  // namespace service::cache
