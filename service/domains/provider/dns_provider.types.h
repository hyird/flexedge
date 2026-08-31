#pragma once

#include <ruvia/web/Model.h>

namespace service::provider {

RUVIA_REQUEST_MODEL(CreateDnsProviderBody, RUVIA_OPTIONAL_FIELD(name, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(provider, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("account_id", accountId, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("api_token", apiToken, ruvia::String));
RUVIA_REQUEST_MODEL(UpdateDnsProviderBody, RUVIA_OPTIONAL_FIELD(name, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("api_token", apiToken, ruvia::String));
RUVIA_RESPONSE_MODEL(
    DnsProviderDto, RUVIA_REQUIRED_FIELD(id, ruvia::String),
    RUVIA_REQUIRED_FIELD(revision, ruvia::Int64), RUVIA_REQUIRED_FIELD(name, ruvia::String),
    RUVIA_REQUIRED_FIELD_NAME("account_id", accountId, ruvia::String),
    RUVIA_REQUIRED_FIELD(provider, ruvia::String),
    RUVIA_REQUIRED_FIELD_NAME("token_hint", tokenHint, ruvia::String),
    RUVIA_REQUIRED_FIELD(status, ruvia::String),
    RUVIA_REQUIRED_FIELD_NAME("zone_count", zoneCount, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD_NAME("last_verified_at", lastVerifiedAt, ruvia::String, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("last_error", lastError, ruvia::String, RUVIA_OMIT_EMPTY),
    RUVIA_REQUIRED_FIELD_NAME("created_at", createdAt, ruvia::String),
    RUVIA_REQUIRED_FIELD_NAME("updated_at", updatedAt, ruvia::String));
RUVIA_RESPONSE_MODEL(DnsProviderPageDataDto,
                     RUVIA_REQUIRED_FIELD(list, ruvia::Array<DnsProviderDto>),
                     RUVIA_REQUIRED_FIELD(total, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD(page, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD_NAME("page_size", pageSize, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD_NAME("total_pages", totalPages, ruvia::Int64));
RUVIA_RESPONSE_MODEL(DnsProviderPageResponse, RUVIA_REQUIRED_FIELD(code, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD(message, ruvia::String),
                     RUVIA_REQUIRED_FIELD(data, DnsProviderPageDataDto));
RUVIA_RESPONSE_MODEL(DnsProviderDetailResponse, RUVIA_REQUIRED_FIELD(code, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD(message, ruvia::String),
                     RUVIA_REQUIRED_FIELD(data, DnsProviderDto));

} // namespace service::provider
