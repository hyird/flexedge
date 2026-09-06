#pragma once

#include <cstdint>

#include "service/domains/website/website.types.h"
#include "service/features/geoip/xdb_database.h"

namespace service::website {

template <typename Row>
inline void fillWebsiteAccessLog(WebsiteAccessLogDto& item, const Row& row) {
    item.set<"id">(row[0].value().value_or(""));
    item.set<"occurredAt">(row[1].value().value_or(""));
    item.set<"nodeId">(row[2].value().value_or(""));
    item.set<"nodeName">(row[3].value().value_or(""));
    item.set<"protocol">(row[5].value().value_or(""));
    item.set<"method">(row[6].value().value_or(""));
    item.set<"host">(row[7].value().value_or(""));
    item.set<"target">(row[8].value().value_or(""));
    item.set<"statusCode">(row[9].template as<std::int64_t>().value_or(0));
    item.set<"requestBytes">(row[10].template as<std::int64_t>().value_or(0));
    item.set<"responseBytes">(row[11].template as<std::int64_t>().value_or(0));
    item.set<"durationMs">(row[12].template as<std::int64_t>().value_or(0));
    item.set<"requestBodyTruncated">(row[17].template as<bool>().value_or(false));
    if (const auto value = row[4].value()) {
        item.set<"clientIp">(*value);
        if (const auto location = service::geoip::xdbDatabase().lookup(*value)) {
            item.set<"clientIpLocation">(location->display);
        }
    }
    if (const auto value = row[13].value()) {
        item.set<"userAgent">(*value);
    }
    if (const auto value = row[14].value()) {
        item.set<"referer">(*value);
    }
    if (const auto value = row[15].value()) {
        item.set<"requestHeaders">(*value);
    }
    if (const auto value = row[16].value()) {
        item.set<"requestBody">(*value);
    }
    if (const auto value = row[18].value()) {
        item.set<"tlsFingerprint">(*value);
    }
    if (const auto value = row[19].value()) {
        item.set<"responseHeaders">(*value);
    }
    if (const auto value = row[20].value()) {
        item.set<"queryString">(*value);
    }
    if (const auto value = row[21].value()) {
        item.set<"cookies">(*value);
    }
}

} // namespace service::website
