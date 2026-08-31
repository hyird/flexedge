#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ruvia/core/Task.h>
#include <ruvia/web/redis/Redis.h>

namespace service::log_ingest::queue {

inline constexpr std::string_view kStreamKey{"flexedge:log-deliveries:v3"};
inline constexpr std::string_view kRetainedBytesKey{"flexedge:log-deliveries:v3:bytes"};
inline constexpr std::string_view kConsumerGroup{"flexedge-log-ingest-v3"};
inline constexpr std::string_view kPayloadField{"payload"};
inline constexpr std::int64_t kMaxEntries{200000};
inline constexpr std::int64_t kMaxRetainedBytes{512 * 1024 * 1024};
inline constexpr std::int64_t kMaxDeliveryAttempts{4};
inline constexpr std::uint64_t kReadBatchSize{32};
inline constexpr std::uint64_t kReclaimBatchSize{32};
inline constexpr std::int64_t kReclaimIdleMs{5000};
inline constexpr std::string_view kInitializeScript{R"lua(
local result = redis.pcall('XGROUP', 'CREATE', KEYS[1], ARGV[1], '0', 'MKSTREAM')
if type(result) == 'table' and result.err then
    if string.find(result.err, 'BUSYGROUP', 1, true) then
        return 0
    end
    return redis.error_reply(result.err)
end
return 1
)lua"};
inline constexpr std::string_view kPushScript{R"lua(
local payload_bytes = string.len(ARGV[1])
local maximum_entries = tonumber(ARGV[2])
local maximum_bytes = tonumber(ARGV[3])
local retained_bytes = tonumber(redis.call('GET', KEYS[2]) or '0')
if redis.call('XLEN', KEYS[1]) >= maximum_entries or
   payload_bytes > maximum_bytes - retained_bytes then
    return 0
end
local id = redis.call('XADD', KEYS[1], '*', 'payload', ARGV[1])
redis.call('INCRBY', KEYS[2], payload_bytes)
return id
)lua"};
inline constexpr std::string_view kFinishScript{R"lua(
local pending = redis.call('XPENDING', KEYS[1], ARGV[1], ARGV[2], ARGV[2], '1')
if #pending > 0 and (#pending[1] < 2 or pending[1][2] ~= ARGV[4]) then
    return 0
end
local acknowledged = redis.call('XACK', KEYS[1], ARGV[1], ARGV[2])
if acknowledged == 0 then
    if #redis.call('XRANGE', KEYS[1], ARGV[2], ARGV[2], 'COUNT', 1) == 0 then
        return 1
    end
    return 0
end
if redis.call('XDEL', KEYS[1], ARGV[2]) ~= 1 then
    return 0
end
local retained_bytes = tonumber(redis.call('GET', KEYS[2]) or '0')
local remaining = retained_bytes - tonumber(ARGV[3])
if remaining > 0 then
    redis.call('SET', KEYS[2], remaining)
else
    redis.call('DEL', KEYS[2])
end
return 1
)lua"};

struct Entry final {
    std::string id;
    std::string payload;
    bool valid{};
};

struct AutoClaimResult final {
    std::string nextCursor;
    std::vector<Entry> entries;
};

inline void requireNoError(const ruvia::RedisValue& value, std::string_view operation) {
    if (value.kind() == ruvia::RedisValue::Kind::kError) {
        throw std::runtime_error(std::string(operation) + ": " + std::string(value.error()));
    }
}

inline Entry parseEntry(const ruvia::RedisValue& source) {
    if (source.kind() != ruvia::RedisValue::Kind::kArray) {
        throw std::runtime_error("unexpected Redis log stream entry");
    }
    const auto parts = source.array();
    if (parts.size() != 2 || parts[0].kind() != ruvia::RedisValue::Kind::kString ||
        parts[1].kind() != ruvia::RedisValue::Kind::kArray) {
        throw std::runtime_error("unexpected Redis log stream entry");
    }
    Entry entry{.id = std::string(parts[0].string()), .payload = {}, .valid = false};
    const auto fields = parts[1].array();
    if (fields.size() == 2 && fields[0].kind() == ruvia::RedisValue::Kind::kString &&
        fields[0].string() == kPayloadField &&
        fields[1].kind() == ruvia::RedisValue::Kind::kString) {
        entry.payload = std::string(fields[1].string());
        entry.valid = !entry.payload.empty();
    }
    return entry;
}

inline ruvia::Task<void> initialize(ruvia::RedisHandle redis) {
    const std::array<std::string_view, 1> keys{kStreamKey};
    const std::array<std::string_view, 1> arguments{kConsumerGroup};
    const auto result = co_await redis.eval(kInitializeScript, keys, arguments);
    requireNoError(result, "could not initialize Redis log stream");
    if (result.kind() != ruvia::RedisValue::Kind::kInteger ||
        (result.integer() != 0 && result.integer() != 1)) {
        throw std::runtime_error("unexpected Redis log stream initialization reply");
    }
}

inline ruvia::Task<bool> push(ruvia::RedisHandle redis, std::string_view payload) {
    const auto maximumEntries = std::to_string(kMaxEntries);
    const auto maximumBytes = std::to_string(kMaxRetainedBytes);
    const std::array<std::string_view, 2> keys{kStreamKey, kRetainedBytesKey};
    const std::array<std::string_view, 3> arguments{payload, maximumEntries, maximumBytes};
    const auto result = co_await redis.eval(kPushScript, keys, arguments);
    requireNoError(result, "could not append Redis log stream entry");
    if (result.kind() == ruvia::RedisValue::Kind::kInteger && result.integer() == 0) {
        co_return false;
    }
    if (result.kind() != ruvia::RedisValue::Kind::kString || result.string().empty()) {
        throw std::runtime_error("unexpected Redis log stream append reply");
    }
    co_return true;
}

inline std::vector<Entry> entries(const std::optional<ruvia::RedisXReadGroupResult>& result) {
    std::vector<Entry> output;
    if (!result) {
        return output;
    }
    if (result->streams().size() != 1 || result->streams().front().stream() != kStreamKey) {
        throw std::runtime_error("unexpected Redis log stream read reply");
    }
    const auto& stream = result->streams().front();
    output.reserve(stream.entries().size());
    for (const auto& source : stream.entries()) {
        Entry entry{.id = std::string(source.id()), .payload = {}, .valid = false};
        if (source.fields().size() == 1 && source.fields().front().key() == kPayloadField) {
            entry.payload = std::string(source.fields().front().value());
            entry.valid = !entry.payload.empty();
        }
        output.push_back(std::move(entry));
    }
    return output;
}

inline ruvia::Task<std::vector<Entry>> read(ruvia::RedisHandle redis, std::string_view consumer) {
    const std::array streams{
        ruvia::RedisStreamReadView{.stream = kStreamKey, .id = std::string_view{">"}}};
    ruvia::RedisXReadGroupOptions options{.count = kReadBatchSize};
    options.block = ruvia::RedisBlockWait::forDuration(std::chrono::seconds(1));
    co_return entries(co_await redis.xreadGroup(kConsumerGroup, consumer, streams, options));
}

inline ruvia::Task<void> finish(ruvia::RedisHandle redis, std::string_view consumer,
                                const Entry& entry) {
    const auto payloadBytes = std::to_string(entry.payload.size());
    const std::array<std::string_view, 2> keys{kStreamKey, kRetainedBytesKey};
    const std::array<std::string_view, 4> arguments{kConsumerGroup, entry.id, payloadBytes,
                                                    consumer};
    const auto result = co_await redis.eval(kFinishScript, keys, arguments);
    requireNoError(result, "could not finish Redis log stream entry");
    if (result.kind() != ruvia::RedisValue::Kind::kInteger || result.integer() != 1) {
        throw std::runtime_error("Redis log stream entry is no longer owned by this consumer");
    }
}

inline ruvia::Task<AutoClaimResult> autoClaim(ruvia::RedisHandle redis, std::string_view consumer,
                                              std::string_view cursor) {
    const auto minimumIdle = std::to_string(kReclaimIdleMs);
    const auto count = std::to_string(kReclaimBatchSize);
    const auto value = co_await redis.command("XAUTOCLAIM", kStreamKey, kConsumerGroup, consumer,
                                              minimumIdle, cursor, "COUNT", count);
    requireNoError(value, "could not reclaim Redis log stream entries");
    if (value.kind() != ruvia::RedisValue::Kind::kArray) {
        throw std::runtime_error("unexpected Redis log stream reclaim reply");
    }
    const auto parts = value.array();
    if ((parts.size() != 2 && parts.size() != 3) ||
        parts[0].kind() != ruvia::RedisValue::Kind::kString ||
        parts[1].kind() != ruvia::RedisValue::Kind::kArray ||
        (parts.size() == 3 && parts[2].kind() != ruvia::RedisValue::Kind::kArray)) {
        throw std::runtime_error("unexpected Redis log stream reclaim reply");
    }
    std::vector<Entry> entries;
    entries.reserve(parts[1].array().size());
    for (const auto& source : parts[1].array()) {
        entries.push_back(parseEntry(source));
    }
    co_return AutoClaimResult{.nextCursor = std::string(parts[0].string()),
                              .entries = std::move(entries)};
}

inline ruvia::Task<std::optional<std::int64_t>>
deliveryCount(ruvia::RedisHandle redis, std::string_view consumer, std::string_view entryId) {
    const auto value = co_await redis.command("XPENDING", kStreamKey, kConsumerGroup, entryId,
                                              entryId, "1", consumer);
    requireNoError(value, "could not inspect Redis log stream delivery count");
    if (value.kind() != ruvia::RedisValue::Kind::kArray) {
        throw std::runtime_error("unexpected Redis log stream delivery count reply");
    }
    const auto rows = value.array();
    if (rows.empty()) {
        co_return std::nullopt;
    }
    if (rows.size() != 1 || rows.front().kind() != ruvia::RedisValue::Kind::kArray) {
        throw std::runtime_error("unexpected Redis log stream delivery count reply");
    }
    const auto row = rows.front().array();
    if (row.size() != 4 || row[0].kind() != ruvia::RedisValue::Kind::kString ||
        row[0].string() != entryId || row[3].kind() != ruvia::RedisValue::Kind::kInteger) {
        throw std::runtime_error("unexpected Redis log stream delivery count reply");
    }
    co_return row[3].integer();
}

} // namespace service::log_ingest::queue
