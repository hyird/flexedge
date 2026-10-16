#pragma once

#include <optional>
#include <string_view>

#include "ruvia/web/model.h"

#include "service/common/domain_name.h"
#include "service/common/uuid.h"
#include "service/features/dns_sync/transport.h"

namespace service::dns_zone {

inline bool is_valid_uuid(const ruvia::string& value) {
    return service::common::parse_uuid(std::optional<std::string_view>{value.view()}).has_value();
}

inline bool is_valid_zone_domain(const ruvia::string& value) {
    return service::common::is_hostname(value.view(), false);
}

RUVIA_MODEL(available_dns_zone_dto, RUVIA_REQUIRED_FIELD(domain, ruvia::string),
    RUVIA_REQUIRED_FIELD(status, ruvia::string));
RUVIA_MODEL(available_dns_zone_list_data_dto,
    RUVIA_REQUIRED_FIELD(list, ruvia::array<available_dns_zone_dto>));
RUVIA_MODEL(available_dns_zone_list_response, RUVIA_REQUIRED_FIELD(code, ruvia::int64),
    RUVIA_REQUIRED_FIELD(message, ruvia::string),
    RUVIA_REQUIRED_FIELD(data, available_dns_zone_list_data_dto));

RUVIA_MODEL(dns_zone_option_dto, RUVIA_REQUIRED_FIELD(id, ruvia::string),
    RUVIA_REQUIRED_FIELD(domain, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("dns_provider", dns_provider, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("dns_provider_name", dns_provider_name, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("sync_status", sync_status, ruvia::string),
    RUVIA_REQUIRED_FIELD(available, ruvia::bool_value));
RUVIA_MODEL(dns_zone_option_list_data_dto,
    RUVIA_REQUIRED_FIELD(list, ruvia::array<dns_zone_option_dto>));
RUVIA_MODEL(dns_zone_option_list_response, RUVIA_REQUIRED_FIELD(code, ruvia::int64),
    RUVIA_REQUIRED_FIELD(message, ruvia::string),
    RUVIA_REQUIRED_FIELD(data, dns_zone_option_list_data_dto));
RUVIA_MODEL(dns_zone_collection_response, RUVIA_REQUIRED_FIELD(code, ruvia::int64),
    RUVIA_REQUIRED_FIELD(message, ruvia::string),
    RUVIA_REQUIRED_FIELD(data, ruvia::array<dns_zone_option_dto>));

RUVIA_MODEL(create_dns_zone_body,
    RUVIA_OPTIONAL_FIELD_NAME("dns_provider_id", dns_provider_id, ruvia::string),
    RUVIA_OPTIONAL_FIELD(domain, ruvia::string));

RUVIA_MODEL(projected_dns_record_dto, RUVIA_REQUIRED_FIELD(id, ruvia::string),
    RUVIA_REQUIRED_FIELD(type, ruvia::string),
    RUVIA_REQUIRED_FIELD(name, ruvia::string),
    RUVIA_REQUIRED_FIELD(content, ruvia::string),
    RUVIA_REQUIRED_FIELD(ttl, ruvia::int64),
    RUVIA_OPTIONAL_FIELD(priority, ruvia::int64, RUVIA_OMIT_EMPTY),
    RUVIA_REQUIRED_FIELD(proxied, ruvia::bool_value),
    RUVIA_REQUIRED_FIELD_NAME("line_code", line_code, ruvia::string));

RUVIA_MODEL(dns_line_runtime_dto, RUVIA_REQUIRED_FIELD(code, ruvia::string),
    RUVIA_REQUIRED_FIELD(name, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("display_name", display_name, ruvia::string),
    RUVIA_REQUIRED_FIELD(status, ruvia::string));
RUVIA_MODEL(dns_record_runtime_dto, RUVIA_REQUIRED_FIELD(id, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("sync_status", sync_status, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("synced_revision", synced_revision, ruvia::int64),
    RUVIA_OPTIONAL_FIELD_NAME("last_error", last_error, ruvia::string,
        RUVIA_OMIT_EMPTY));
RUVIA_MODEL(
    dns_zone_runtime_dto,
    RUVIA_REQUIRED_FIELD(lines, ruvia::array<dns_line_runtime_dto>),
    RUVIA_REQUIRED_FIELD_NAME("projected_records", projected_records,
        ruvia::array<projected_dns_record_dto>),
    RUVIA_REQUIRED_FIELD_NAME("record_states", record_states, ruvia::array<dns_record_runtime_dto>));

RUVIA_MODEL(dns_zone_dto, RUVIA_REQUIRED_FIELD(id, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("dns_provider_id", dns_provider_id, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("dns_provider", dns_provider, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("dns_provider_name", dns_provider_name, ruvia::string),
    RUVIA_REQUIRED_FIELD(domain, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("sync_status", sync_status, ruvia::string),
    RUVIA_REQUIRED_FIELD(revision, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("desired_revision", desired_revision, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("synced_revision", synced_revision, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("website_count", website_count, ruvia::int64),
    RUVIA_REQUIRED_FIELD(config, service::dns_sync::zone_config_output),
    RUVIA_REQUIRED_FIELD(runtime, dns_zone_runtime_dto),
    RUVIA_OPTIONAL_FIELD_NAME("last_synced_at", last_synced_at, ruvia::string,
        RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("last_error", last_error, ruvia::string,
        RUVIA_OMIT_EMPTY),
    RUVIA_REQUIRED_FIELD_NAME("created_at", created_at, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("updated_at", updated_at, ruvia::string));
RUVIA_MODEL(dns_zone_page_data_dto, RUVIA_REQUIRED_FIELD(list, ruvia::array<dns_zone_dto>),
    RUVIA_REQUIRED_FIELD(total, ruvia::int64),
    RUVIA_REQUIRED_FIELD(page, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("page_size", page_size, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("total_pages", total_pages, ruvia::int64));
RUVIA_MODEL(dns_zone_page_response, RUVIA_REQUIRED_FIELD(code, ruvia::int64),
    RUVIA_REQUIRED_FIELD(message, ruvia::string),
    RUVIA_REQUIRED_FIELD(data, dns_zone_page_data_dto));
RUVIA_MODEL(dns_zone_detail_response, RUVIA_REQUIRED_FIELD(code, ruvia::int64),
    RUVIA_REQUIRED_FIELD(message, ruvia::string),
    RUVIA_REQUIRED_FIELD(data, dns_zone_dto));

}  // namespace service::dns_zone
