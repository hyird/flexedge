#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#include "service/features/log_ingest/envelope.h"
#include "service/features/log_ingest/log_entity.h"

namespace service::log_ingest {

class invalid_delivery final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct log_rows final {
    std::vector<access_log_entity> access_{};
    std::vector<node_log_entity> node_{};
    std::vector<std::string> websites_{};
};

inline std::string occurred_timestamp(std::uint64_t value, std::int64_t received_unix_ms) {
    const auto maximum = static_cast<std::uint64_t>(received_unix_ms + 5 * 60 * 1000);
    const auto milliseconds = value == 0 || value > maximum ? received_unix_ms : static_cast<std::int64_t>(value);
    const std::chrono::sys_time<std::chrono::milliseconds> point{std::chrono::milliseconds(milliseconds)};
    const auto day = std::chrono::floor<std::chrono::days>(point);
    const std::chrono::year_month_day date{day};
    const std::chrono::hh_mm_ss clock{point - day};
    std::array<char, 40> text{};
    const auto length = std::snprintf(text.data(), text.size(), "%04d-%02u-%02uT%02lld:%02lld:%02lld.%03lldZ",
        static_cast<int>(date.year()), static_cast<unsigned>(date.month()), static_cast<unsigned>(date.day()),
        static_cast<long long>(clock.hours().count()), static_cast<long long>(clock.minutes().count()),
        static_cast<long long>(clock.seconds().count()), static_cast<long long>(clock.subseconds().count()));
    if (length <= 0 || static_cast<std::size_t>(length) >= text.size()) {
        throw invalid_delivery("invalid log timestamp");
    }
    return {text.data(), static_cast<std::size_t>(length)};
}

// Owns every field it maps; conversion neither reads nor writes external state.
// The database supplies created_at for all rows in the same transaction.
inline log_rows to_log_rows(const v2::QueuedLogDelivery& envelope, std::int64_t received_unix_ms) {
    if (!valid_envelope(envelope)) {
        throw invalid_delivery("invalid queued log delivery");
    }
    log_rows rows;
    for (const auto& event : envelope.delivery().events()) {
        if (event.has_access_log()) {
            const auto& access = event.access_log();
            auto& row = rows.access_.emplace_back();
            row.set<"tenant_id">(envelope.tenant_id());
            row.set<"occurred_at">(occurred_timestamp(access.occurred_unix_ms(), received_unix_ms));
            row.set<"id">(access.id());
            row.set<"node_id">(envelope.delivery().node_id());
            row.set<"website_id">(access.website_id());
            row.set<"client_ip">(access.client_ip());
            row.set<"protocol">(access.protocol());
            row.set<"method">(access.method());
            row.set<"host">(access.host());
            row.set<"target">(access.target());
            row.set<"status_code">(static_cast<std::int32_t>(access.status_code()));
            row.set<"request_bytes">(static_cast<std::int64_t>(access.has_total_request_bytes() ? access.total_request_bytes() : access.request_bytes()));
            row.set<"response_bytes">(static_cast<std::int64_t>(access.has_total_response_bytes() ? access.total_response_bytes() : access.response_bytes()));
            row.set<"duration_ms">(static_cast<std::int64_t>(access.has_total_duration_ms() ? access.total_duration_ms() : access.duration_ms()));
            row.set<"request_type">(access.request_type());
            row.set<"end_reason">(access.end_reason());
            if (access.started_unix_ms()) {
                row.set<"started_at">(occurred_timestamp(access.started_unix_ms(), received_unix_ms));
            }
            if (access.has_first_byte_ms()) {
                row.set<"first_byte_ms">(static_cast<std::int64_t>(access.first_byte_ms()));
            }
            row.set<"user_agent">(access.user_agent());
            row.set<"referer">(access.referer());
            row.set<"request_headers">(access.request_headers());
            row.set<"request_body">(access.request_body());
            row.set<"request_body_truncated">(access.request_body_truncated());
            row.set<"tls_fingerprint">(access.tls_fingerprint());
            row.set<"response_headers">(access.response_headers());
            row.set<"query_string">(access.query_string());
            row.set<"cookies">(access.cookies());
            row.set<"cache_status">(access.cache_revalidated() ? "revalidated" : access.cache_status());
            row.set<"cache_reason">(access.cache_reason());
            if (access.has_waf_anomaly_score()) {
                row.set<"waf_anomaly_score">(static_cast<std::int64_t>(access.waf_anomaly_score()));
            }
            rows.websites_.push_back(access.website_id());
        } else {
            const auto& item = event.node_log();
            auto& row = rows.node_.emplace_back();
            row.set<"tenant_id">(envelope.tenant_id());
            row.set<"occurred_at">(occurred_timestamp(item.occurred_unix_ms(), received_unix_ms));
            row.set<"id">(item.id());
            row.set<"node_id">(envelope.delivery().node_id());
            row.set<"level">(item.level());
            row.set<"category">(item.category());
            row.set<"message">(item.message());
        }
    }
    std::sort(rows.websites_.begin(), rows.websites_.end());
    rows.websites_.erase(std::unique(rows.websites_.begin(), rows.websites_.end()), rows.websites_.end());
    return rows;
}

}  // namespace service::log_ingest
