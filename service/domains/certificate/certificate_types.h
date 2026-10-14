#pragma once

#include "ruvia/web/model.h"

#include "service/features/certificate/config_transport.h"

namespace service::certificate {

RUVIA_MODEL(create_certificate_body,
    RUVIA_OPTIONAL_FIELD(domain, ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("certificate_provider_id", certificate_provider_id,
        ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("dns_zone_id", dns_zone_id, ruvia::string),
    RUVIA_OPTIONAL_FIELD(config,
        service::certificate_issuance::certificate_config_input));
RUVIA_MODEL(
    certificate_dto, RUVIA_REQUIRED_FIELD(id, ruvia::string),
    RUVIA_REQUIRED_FIELD(revision, ruvia::int64),
    RUVIA_REQUIRED_FIELD(domains, ruvia::array<ruvia::string>),
    RUVIA_REQUIRED_FIELD(issuer, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("certificate_provider_id", certificate_provider_id, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("certificate_provider", certificate_provider, ruvia::string),
    RUVIA_REQUIRED_FIELD(status, ruvia::string), RUVIA_REQUIRED_FIELD(usable, ruvia::bool_value),
    RUVIA_REQUIRED_FIELD(config, service::certificate_issuance::certificate_config_output),
    RUVIA_REQUIRED_FIELD_NAME("dns_zone_id", dns_zone_id, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("dns_zone_domain", dns_zone_domain, ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("not_before", not_before, ruvia::string, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("expires_at", expires_at, ruvia::string, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("remaining_days", remaining_days, ruvia::int64, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("last_error", last_error, ruvia::string, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("serial_number", serial_number, ruvia::string, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("fingerprint_sha256", fingerprint_sha256, ruvia::string,
        RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("last_issued_at", last_issued_at, ruvia::string, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("sync_status", sync_status, ruvia::string, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("sync_count_fails", sync_count_fails, ruvia::int64, RUVIA_OMIT_EMPTY),
    RUVIA_REQUIRED_FIELD_NAME("website_count", website_count, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("created_at", created_at, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("updated_at", updated_at, ruvia::string));
RUVIA_MODEL(certificate_page_data_dto,
    RUVIA_REQUIRED_FIELD(list, ruvia::array<certificate_dto>),
    RUVIA_REQUIRED_FIELD(total, ruvia::int64),
    RUVIA_REQUIRED_FIELD(page, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("page_size", page_size, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("total_pages", total_pages, ruvia::int64));
RUVIA_MODEL(certificate_page_response, RUVIA_REQUIRED_FIELD(code, ruvia::int64),
    RUVIA_REQUIRED_FIELD(message, ruvia::string),
    RUVIA_REQUIRED_FIELD(data, certificate_page_data_dto));
RUVIA_MODEL(certificate_detail_response, RUVIA_REQUIRED_FIELD(code, ruvia::int64),
    RUVIA_REQUIRED_FIELD(message, ruvia::string),
    RUVIA_REQUIRED_FIELD(data, certificate_dto));
RUVIA_MODEL(certificate_options_response, RUVIA_REQUIRED_FIELD(code, ruvia::int64),
    RUVIA_REQUIRED_FIELD(message, ruvia::string),
    RUVIA_REQUIRED_FIELD(data, ruvia::array<certificate_dto>));
}  // namespace service::certificate
