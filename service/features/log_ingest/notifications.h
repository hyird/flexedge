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

inline constexpr std::string_view kStreamKey{"flexedge:log-notifications:v2"};
inline constexpr std::int64_t kMaxEntries{200000};
inline constexpr std::uint64_t kReadBatchSize{64};
inline constexpr auto kReadBlock = std::chrono::seconds(15);
inline constexpr auto kReadTimeout = std::chrono::seconds(20);

enum class LogResourceType {
    access,
    node,
};

[[nodiscard]] inline constexpr std::string_view resourceTypeName(LogResourceType resourceType) {
    switch (resourceType) {
    case LogResourceType::access:
        return "access";
    case LogResourceType::node:
        return "node";
    }
    throw std::invalid_argument("unsupported log notification resource type");
}

[[nodiscard]] inline std::optional<LogResourceType> parseResourceType(std::string_view value) {
    if (value == "access") {
        return LogResourceType::access;
    }
    if (value == "node") {
        return LogResourceType::node;
    }
    return std::nullopt;
}

struct Notification final {
    std::string id;
    std::string tenantId;
    LogResourceType resourceType{};
    std::string resourceId;
};

struct ReadBatch final {
    std::vector<Notification> notifications;
    std::optional<std::string> cursor;
};

inline void requireNoError(const ruvia::RedisValue& value, std::string_view operation) {
    if (value.kind() == ruvia::RedisValue::Kind::kError) {
        throw std::runtime_error(std::string(operation) + ": " + std::string(value.error()));
    }
}

inline ruvia::Task<void> publish(ruvia::RedisHandle redis, LogResourceType resourceType,
                                 std::string_view tenantId, std::string_view resourceId) {
    if (tenantId.empty() || resourceId.empty()) {
        co_return;
    }
    const auto maximumEntries = std::to_string(kMaxEntries);
    const auto result = co_await redis.command(
        "XADD", kStreamKey, "MAXLEN", "~", maximumEntries, "*", "tenant_id", tenantId,
        "resource_type", resourceTypeName(resourceType), "resource_id", resourceId);
    requireNoError(result, "could not publish log notification");
    if (result.kind() != ruvia::RedisValue::Kind::kString || result.string().empty()) {
        throw std::runtime_error("unexpected log notification publish reply");
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

inline std::optional<std::string> fieldValue(std::span<const ruvia::RedisValue> fields,
                                             std::string_view name) {
    if (fields.size() % 2 != 0) {
        return std::nullopt;
    }
    for (std::size_t index = 0; index < fields.size(); index += 2) {
        if (fields[index].kind() != ruvia::RedisValue::Kind::kString ||
            fields[index + 1].kind() != ruvia::RedisValue::Kind::kString) {
            return std::nullopt;
        }
        if (fields[index].string() == name) {
            return std::string(fields[index + 1].string());
        }
    }
    return std::nullopt;
}

inline std::optional<Notification> parseEntry(const ruvia::RedisValue& value) {
    if (value.kind() != ruvia::RedisValue::Kind::kArray) {
        return std::nullopt;
    }
    const auto entry = value.array();
    if (entry.size() != 2 || entry[0].kind() != ruvia::RedisValue::Kind::kString ||
        entry[1].kind() != ruvia::RedisValue::Kind::kArray) {
        return std::nullopt;
    }
    const auto fields = entry[1].array();
    const auto tenantId = fieldValue(fields, "tenant_id");
    const auto resourceType = fieldValue(fields, "resource_type");
    const auto resourceId = fieldValue(fields, "resource_id");
    if (!tenantId || !resourceType || !resourceId || tenantId->empty() || resourceId->empty()) {
        return std::nullopt;
    }
    const auto parsedResourceType = parseResourceType(*resourceType);
    if (!parsedResourceType) {
        return std::nullopt;
    }
    return Notification{
        .id = std::string(entry[0].string()),
        .tenantId = std::move(*tenantId),
        .resourceType = *parsedResourceType,
        .resourceId = std::move(*resourceId),
    };
}

inline std::optional<std::string> entryId(const ruvia::RedisValue& value) {
    if (value.kind() != ruvia::RedisValue::Kind::kArray) {
        return std::nullopt;
    }
    const auto entry = value.array();
    if (entry.size() != 2 || entry[0].kind() != ruvia::RedisValue::Kind::kString) {
        return std::nullopt;
    }
    return std::string(entry[0].string());
}

inline ReadBatch parseReadResult(const ruvia::RedisValue& value) {
    requireNoError(value, "could not read log notifications");
    ReadBatch output;
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
    output.notifications.reserve(entries.size());
    for (const auto& entry : entries) {
        const auto id = entryId(entry);
        if (!id) {
            throw std::runtime_error("unexpected log notification entry");
        }
        output.cursor = *id;
        if (const auto parsed = parseEntry(entry)) {
            output.notifications.push_back(*parsed);
        }
    }
    return output;
}

inline ruvia::Task<ReadBatch> read(ruvia::RedisHandle redis, std::string_view cursor) {
    const auto blockMs =
        std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(kReadBlock).count());
    const auto count = std::to_string(kReadBatchSize);
    const auto redisWithTimeout = redis.withOptions({.timeout = kReadTimeout});
    co_return parseReadResult(co_await redisWithTimeout.command(
        "XREAD", "BLOCK", blockMs, "COUNT", count, "STREAMS", kStreamKey, cursor));
}

} // namespace service::log_ingest::notifications
