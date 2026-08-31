#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <ruvia/core/Task.h>
#include <ruvia/web/redis/Redis.h>

namespace service::log_ingest::notifications {

inline constexpr std::string_view kStreamKey{"flexedge:log-notifications:v1"};
inline constexpr std::string_view kKindAccess{"access"};
inline constexpr std::string_view kKindNode{"node"};
inline constexpr std::int64_t kMaxEntries{200000};
inline constexpr std::uint64_t kReadBatchSize{64};
inline constexpr auto kReadBlock = std::chrono::seconds(15);
inline constexpr auto kReadTimeout = std::chrono::seconds(20);

struct Notification final {
    std::string id;
    std::string tenantId;
    std::string kind;
    std::string websiteId;
    std::string nodeId;
};

inline void requireNoError(const ruvia::RedisValue& value, std::string_view operation) {
    if (value.kind() == ruvia::RedisValue::Kind::kError) {
        throw std::runtime_error(std::string(operation) + ": " + std::string(value.error()));
    }
}

inline ruvia::Task<void> publishAccess(ruvia::RedisHandle redis, std::string_view tenantId,
                                       std::string_view websiteId) {
    if (tenantId.empty() || websiteId.empty()) {
        co_return;
    }
    const auto maximumEntries = std::to_string(kMaxEntries);
    const auto result =
        co_await redis.command("XADD", kStreamKey, "MAXLEN", "~", maximumEntries, "*", "tenant_id",
                               tenantId, "kind", kKindAccess, "website_id", websiteId);
    requireNoError(result, "could not publish access log notification");
    if (result.kind() != ruvia::RedisValue::Kind::kString || result.string().empty()) {
        throw std::runtime_error("unexpected access log notification publish reply");
    }
}

inline ruvia::Task<void> publishNode(ruvia::RedisHandle redis, std::string_view tenantId,
                                     std::string_view nodeId) {
    if (tenantId.empty() || nodeId.empty()) {
        co_return;
    }
    const auto maximumEntries = std::to_string(kMaxEntries);
    const auto result =
        co_await redis.command("XADD", kStreamKey, "MAXLEN", "~", maximumEntries, "*", "tenant_id",
                               tenantId, "kind", kKindNode, "node_id", nodeId);
    requireNoError(result, "could not publish node log notification");
    if (result.kind() != ruvia::RedisValue::Kind::kString || result.string().empty()) {
        throw std::runtime_error("unexpected node log notification publish reply");
    }
}

inline std::string currentEntryId(const ruvia::RedisValue& value) {
    requireNoError(value, "could not read log notification cursor");
    if (value.null()) {
        return "0-0";
    }
    if (value.kind() != ruvia::RedisValue::Kind::kArray) {
        throw std::runtime_error("unexpected log notification cursor reply");
    }
    const auto entries = value.array();
    if (entries.empty()) {
        return "0-0";
    }
    const auto entry = entries.front().array();
    if (entry.size() != 2 || entry.front().kind() != ruvia::RedisValue::Kind::kString) {
        throw std::runtime_error("unexpected log notification cursor entry");
    }
    return std::string(entry.front().string());
}

inline ruvia::Task<std::string> currentCursor(ruvia::RedisHandle redis) {
    co_return currentEntryId(
        co_await redis.command("XREVRANGE", kStreamKey, "+", "-", "COUNT", "1"));
}

inline std::string fieldValue(std::span<const ruvia::RedisValue> fields, std::string_view name) {
    if (fields.size() % 2 != 0) {
        throw std::runtime_error("unexpected log notification field list");
    }
    for (std::size_t index = 0; index < fields.size(); index += 2) {
        if (fields[index].kind() != ruvia::RedisValue::Kind::kString ||
            fields[index + 1].kind() != ruvia::RedisValue::Kind::kString) {
            throw std::runtime_error("unexpected log notification field value");
        }
        if (fields[index].string() == name) {
            return std::string(fields[index + 1].string());
        }
    }
    return {};
}

inline Notification parseEntry(const ruvia::RedisValue& value) {
    if (value.kind() != ruvia::RedisValue::Kind::kArray) {
        throw std::runtime_error("unexpected log notification entry");
    }
    const auto entry = value.array();
    if (entry.size() != 2 || entry[0].kind() != ruvia::RedisValue::Kind::kString ||
        entry[1].kind() != ruvia::RedisValue::Kind::kArray) {
        throw std::runtime_error("unexpected log notification entry");
    }
    const auto fields = entry[1].array();
    return {
        .id = std::string(entry[0].string()),
        .tenantId = fieldValue(fields, "tenant_id"),
        .kind = fieldValue(fields, "kind"),
        .websiteId = fieldValue(fields, "website_id"),
        .nodeId = fieldValue(fields, "node_id"),
    };
}

inline std::vector<Notification> parseReadResult(const ruvia::RedisValue& value) {
    requireNoError(value, "could not read log notifications");
    std::vector<Notification> output;
    if (value.null()) {
        return output;
    }
    if (value.kind() != ruvia::RedisValue::Kind::kArray) {
        throw std::runtime_error("unexpected log notification read reply");
    }
    const auto streams = value.array();
    if (streams.empty()) {
        return output;
    }
    if (streams.size() != 1 || streams.front().kind() != ruvia::RedisValue::Kind::kArray) {
        throw std::runtime_error("unexpected log notification read reply");
    }
    const auto stream = streams.front().array();
    if (stream.size() != 2 || stream[0].kind() != ruvia::RedisValue::Kind::kString ||
        stream[0].string() != kStreamKey || stream[1].kind() != ruvia::RedisValue::Kind::kArray) {
        throw std::runtime_error("unexpected log notification stream reply");
    }
    const auto entries = stream[1].array();
    output.reserve(entries.size());
    for (const auto& entry : entries) {
        output.push_back(parseEntry(entry));
    }
    return output;
}

inline ruvia::Task<std::vector<Notification>> read(ruvia::RedisHandle redis,
                                                   std::string_view cursor) {
    const auto blockMs =
        std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(kReadBlock).count());
    const auto count = std::to_string(kReadBatchSize);
    const auto redisWithTimeout = redis.withOptions({.timeout = kReadTimeout});
    co_return parseReadResult(co_await redisWithTimeout.command(
        "XREAD", "BLOCK", blockMs, "COUNT", count, "STREAMS", kStreamKey, cursor));
}

inline bool matchesAccess(const Notification& notification, std::string_view tenantId,
                          std::string_view websiteId) {
    return std::string_view{notification.kind} == kKindAccess &&
           std::string_view{notification.tenantId} == tenantId &&
           std::string_view{notification.websiteId} == websiteId;
}

inline bool matchesNode(const Notification& notification, std::string_view tenantId,
                        std::string_view nodeId) {
    return std::string_view{notification.kind} == kKindNode &&
           std::string_view{notification.tenantId} == tenantId &&
           std::string_view{notification.nodeId} == nodeId;
}

} // namespace service::log_ingest::notifications
