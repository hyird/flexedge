#pragma once

#include <ruvia/web/model.h>

namespace service::provider {

RUVIA_MODEL(create_dns_provider_body,
    RUVIA_OPTIONAL_FIELD(name, ruvia::string),
    RUVIA_OPTIONAL_FIELD(provider, ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("account_id", account_id, ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("api_token", api_token, ruvia::string));
RUVIA_MODEL(update_dns_provider_body,
    RUVIA_OPTIONAL_FIELD(name, ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("api_token", api_token, ruvia::string));
RUVIA_MODEL(
    dns_provider_dto, RUVIA_REQUIRED_FIELD(id, ruvia::string),
    RUVIA_REQUIRED_FIELD(revision, ruvia::int64), RUVIA_REQUIRED_FIELD(name, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("account_id", account_id, ruvia::string),
    RUVIA_REQUIRED_FIELD(provider, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("token_hint", token_hint, ruvia::string),
    RUVIA_REQUIRED_FIELD(status, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("zone_count", zone_count, ruvia::int64),
    RUVIA_OPTIONAL_FIELD_NAME("last_verified_at", last_verified_at, ruvia::string, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("last_error", last_error, ruvia::string, RUVIA_OMIT_EMPTY),
    RUVIA_REQUIRED_FIELD_NAME("created_at", created_at, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("updated_at", updated_at, ruvia::string));
RUVIA_MODEL(dns_provider_page_data_dto,
    RUVIA_REQUIRED_FIELD(list, ruvia::array<dns_provider_dto>),
    RUVIA_REQUIRED_FIELD(total, ruvia::int64),
    RUVIA_REQUIRED_FIELD(page, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("page_size", page_size, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("total_pages", total_pages, ruvia::int64));
RUVIA_MODEL(dns_provider_page_response, RUVIA_REQUIRED_FIELD(code, ruvia::int64),
    RUVIA_REQUIRED_FIELD(message, ruvia::string),
    RUVIA_REQUIRED_FIELD(data, dns_provider_page_data_dto));
RUVIA_MODEL(dns_provider_detail_response, RUVIA_REQUIRED_FIELD(code, ruvia::int64),
    RUVIA_REQUIRED_FIELD(message, ruvia::string),
    RUVIA_REQUIRED_FIELD(data, dns_provider_dto));
RUVIA_MODEL(dns_provider_options_response, RUVIA_REQUIRED_FIELD(code, ruvia::int64),
    RUVIA_REQUIRED_FIELD(message, ruvia::string),
    RUVIA_REQUIRED_FIELD(data, ruvia::array<dns_provider_dto>));

}  // namespace service::provider
