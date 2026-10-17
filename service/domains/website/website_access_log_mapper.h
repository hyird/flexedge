#pragma once

#include <cstdint>

#include "service/domains/website/website_types.h"
#include "service/features/geoip/runtime_database.h"

namespace service::website {

template <typename row_type>
inline void fill_website_access_log(website_access_log_dto& item, const row_type& row) {
    item.set<"id">(row[0].value().value_or(""));
    item.set<"occurred_at">(row[1].value().value_or(""));
    item.set<"node_id">(row[2].value().value_or(""));
    item.set<"node_name">(row[3].value().value_or(""));
    item.set<"protocol">(row[5].value().value_or(""));
    item.set<"method">(row[6].value().value_or(""));
    item.set<"host">(row[7].value().value_or(""));
    item.set<"target">(row[8].value().value_or(""));
    item.set<"status_code">(row[9].template as<std::int64_t>().value_or(0));
    item.set<"request_bytes">(row[10].template as<std::int64_t>().value_or(0));
    item.set<"response_bytes">(row[11].template as<std::int64_t>().value_or(0));
    item.set<"duration_ms">(row[12].template as<std::int64_t>().value_or(0));
    item.set<"request_body_truncated">(row[17].template as<bool>().value_or(false));
    item.set<"cache_status">(row[22].value().value_or(""));
    item.set<"cache_reason">(row[23].value().value_or(""));
    if (const auto value = row[4].value()) {
        item.set<"client_ip">(*value);
        if (const auto location = service::geoip::get_xdb_database().lookup(*value)) {
            item.set<"client_ip_location">(location->display_);
            if (!location->asn_.empty()) {
                item.set<"client_ip_asn">(location->asn_);
            }
            if (!location->as_name_.empty()) {
                item.set<"client_ip_as_name">(location->as_name_);
            }
        }
    }
    if (const auto value = row[13].value()) {
        item.set<"user_agent">(*value);
    }
    if (const auto value = row[14].value()) {
        item.set<"referer">(*value);
    }
    if (const auto value = row[15].value()) {
        item.set<"request_headers">(*value);
    }
    if (const auto value = row[16].value()) {
        item.set<"request_body">(*value);
    }
    if (const auto value = row[18].value()) {
        item.set<"tls_fingerprint">(*value);
    }
    if (const auto value = row[19].value()) {
        item.set<"response_headers">(*value);
    }
    if (const auto value = row[20].value()) {
        item.set<"query_string">(*value);
    }
    if (const auto value = row[21].value()) {
        item.set<"cookies">(*value);
    }
}

}  // namespace service::website
