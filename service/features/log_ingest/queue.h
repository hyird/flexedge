#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/redis/redis.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "service/features/background/work_available.h"
#include "service/utils/redis_reply.h"

namespace service::log_ingest::queue {

using service::utils::require_redis_success;

inline constexpr std::string_view stream_key{"flexedge:log-deliveries:v3"};
inline constexpr std::string_view retained_bytes_key{"flexedge:log-deliveries:v3:bytes"};
inline constexpr std::string_view retry_schedule_key{"flexedge:log-deliveries:v3:retry-at"};
inline constexpr std::string_view retry_attempts_key{"flexedge:log-deliveries:v3:attempts"};
inline constexpr std::string_view failed_stream_key{"flexedge:log-deliveries:v3:failed"};
inline constexpr std::string_view failed_bytes_key{"flexedge:log-deliveries:v3:failed:bytes"};
inline constexpr std::string_view statistics_key{"flexedge:log-deliveries:v3:statistics"};
inline constexpr std::string_view consumer_group{"flexedge-log-ingest-v3"};
inline constexpr std::string_view payload_field{"payload"};
inline constexpr std::int64_t max_entries{200000};
inline constexpr std::int64_t max_retained_bytes{512 * 1024 * 1024};
inline constexpr std::int64_t max_failed_entries{10000};
inline constexpr std::int64_t max_failed_bytes{64 * 1024 * 1024};
inline constexpr std::int64_t retry_initial_ms{1000};
inline constexpr std::int64_t retry_maximum_ms{60000};
// A delivery already contains a bounded batch of log records. Claim one delivery
// at a time so a slow database cannot leave the rest of a claimed batch idle.
inline constexpr std::uint64_t read_batch_size{1};
inline constexpr std::uint64_t reclaim_batch_size{1};
inline constexpr std::int64_t reclaim_idle_ms{5 * 60 * 1000};
inline constexpr std::string_view initialize_script{R"lua(
local result = redis.pcall('XGROUP', 'CREATE', KEYS[1], ARGV[1], '0', 'MKSTREAM')
if type(result) == 'table' and result.err then
    if string.find(result.err, 'BUSYGROUP', 1, true) then
        return 0
    end
    return redis.error_reply(result.err)
end
return 1
)lua"};
inline constexpr std::string_view push_script{R"lua(
local wake_type = redis.call('TYPE', KEYS[3]).ok
if wake_type ~= 'none' and wake_type ~= 'stream' then return redis.error_reply('invalid work availability key type') end
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
redis.call('XADD', KEYS[3], 'MAXLEN', '~', 10000, '*', 'type', 'log-ingest')
return id
)lua"};
inline constexpr std::string_view finish_script{R"lua(
local kinds = {'stream','string','zset','hash','hash','stream'}
for index, kind in ipairs(kinds) do
    local actual = redis.call('TYPE', KEYS[index]).ok
    if actual ~= 'none' and actual ~= kind then return redis.error_reply('invalid log completion key type') end
end
local pending = redis.call('XPENDING', KEYS[1], ARGV[1], ARGV[2], ARGV[2], '1')
if #pending > 0 and (#pending[1] < 2 or pending[1][2] ~= ARGV[3]) then
    return 0
end
local rows = redis.call('XRANGE', KEYS[1], ARGV[2], ARGV[2], 'COUNT', 1)
if #rows == 0 then return 1 end
if #pending == 0 then return 0 end
local payload_bytes = 0
for index = 1, #rows[1][2], 2 do
    if rows[1][2][index] == 'payload' then
        payload_bytes = string.len(rows[1][2][index + 1])
    end
end
local acknowledged = redis.call('XACK', KEYS[1], ARGV[1], ARGV[2])
if acknowledged == 0 then return 0 end
redis.call('XDEL', KEYS[1], ARGV[2])
local retained_bytes = tonumber(redis.call('GET', KEYS[2]) or '0')
local remaining = retained_bytes - payload_bytes
if remaining > 0 then
    redis.call('SET', KEYS[2], remaining)
else
    redis.call('DEL', KEYS[2])
end
redis.call('ZREM', KEYS[3], ARGV[2])
redis.call('HDEL', KEYS[4], ARGV[2])
redis.call('HINCRBY', KEYS[5], 'finished', 1)
redis.call('XADD', KEYS[6], 'MAXLEN', '~', 10000, '*', 'type', 'outbox')
return 1
)lua"};

inline constexpr std::string_view retry_script{R"lua(
local pending = redis.call('XPENDING', KEYS[1], ARGV[1], ARGV[2], ARGV[2], '1')
if #pending == 0 or pending[1][2] ~= ARGV[3] then return 0 end
local attempt = redis.call('HINCRBY', KEYS[3], ARGV[2], 1)
local delay = math.min(tonumber(ARGV[5]), tonumber(ARGV[4]) * 2 ^ math.min(attempt - 1, 16))
local clock = redis.call('TIME')
local now = tonumber(clock[1]) * 1000 + math.floor(tonumber(clock[2]) / 1000)
redis.call('ZADD', KEYS[2], now + delay, ARGV[2])
redis.call('XCLAIM', KEYS[1], ARGV[1], ARGV[3], 0, ARGV[2], 'JUSTID')
redis.call('HINCRBY', KEYS[4], 'retried', 1)
return delay
)lua"};

inline constexpr std::string_view claim_retry_script{R"lua(
local clock = redis.call('TIME')
local now = tonumber(clock[1]) * 1000 + math.floor(tonumber(clock[2]) / 1000)
local ids = redis.call('ZRANGEBYSCORE', KEYS[2], '-inf', now, 'LIMIT', 0, ARGV[3])
local entries = {}
for _, id in ipairs(ids) do
    local claimed = redis.call('XCLAIM', KEYS[1], ARGV[1], ARGV[2], 0, id)
    redis.call('ZREM', KEYS[2], id)
    if #claimed > 0 then
        entries[#entries + 1] = claimed[1]
    else
        redis.call('HDEL', KEYS[3], id)
    end
end
return entries
)lua"};

inline constexpr std::string_view fail_script{R"lua(
local pending = redis.call('XPENDING', KEYS[1], ARGV[1], ARGV[2], ARGV[2], '1')
if #pending == 0 or pending[1][2] ~= ARGV[3] then return -1 end
local rows = redis.call('XRANGE', KEYS[1], ARGV[2], ARGV[2], 'COUNT', 1)
if #rows == 0 then return -1 end
local payload = ''
for index = 1, #rows[1][2], 2 do
    if rows[1][2][index] == 'payload' then payload = rows[1][2][index + 1] end
end
local reason = ARGV[4]
local retained_bytes = tonumber(redis.call('GET', KEYS[6]) or '0')
local record_bytes = string.len(payload) + string.len(reason) + string.len(ARGV[2]) + 64
if redis.call('XLEN', KEYS[5]) >= tonumber(ARGV[5]) or
   record_bytes > tonumber(ARGV[6]) - retained_bytes then
    return 0
end
local clock = redis.call('TIME')
local now = tonumber(clock[1]) * 1000 + math.floor(tonumber(clock[2]) / 1000)
local attempts = redis.call('HGET', KEYS[4], ARGV[2]) or '0'
redis.call('XADD', KEYS[5], '*', 'source_id', ARGV[2], 'payload', payload,
    'reason', reason, 'attempts', attempts, 'failed_unix_ms', tostring(now),
    'retained_bytes', tostring(record_bytes))
redis.call('INCRBY', KEYS[6], record_bytes)
redis.call('XACK', KEYS[1], ARGV[1], ARGV[2])
redis.call('XDEL', KEYS[1], ARGV[2])
local remaining = tonumber(redis.call('GET', KEYS[2]) or '0') - string.len(payload)
if remaining > 0 then redis.call('SET', KEYS[2], remaining) else redis.call('DEL', KEYS[2]) end
redis.call('ZREM', KEYS[3], ARGV[2])
redis.call('HDEL', KEYS[4], ARGV[2])
redis.call('HINCRBY', KEYS[7], 'failed', 1)
return 1
)lua"};

inline constexpr std::string_view replay_failed_script{R"lua(
local wake_type = redis.call('TYPE', KEYS[6]).ok
if wake_type ~= 'none' and wake_type ~= 'stream' then return redis.error_reply('invalid work availability key type') end
local rows = redis.call('XRANGE', KEYS[1], ARGV[1], ARGV[1], 'COUNT', 1)
if #rows == 0 then return 0 end
local fields = {}
for index = 1, #rows[1][2], 2 do fields[rows[1][2][index]] = rows[1][2][index + 1] end
if fields['payload'] == nil or fields['retained_bytes'] == nil then
    return redis.error_reply('invalid failed log delivery')
end
local payload_bytes = string.len(fields['payload'])
local retained_bytes = tonumber(redis.call('GET', KEYS[4]) or '0')
if redis.call('XLEN', KEYS[3]) >= tonumber(ARGV[2]) or
   payload_bytes > tonumber(ARGV[3]) - retained_bytes then return 0 end
local id = redis.call('XADD', KEYS[3], '*', 'payload', fields['payload'])
redis.call('INCRBY', KEYS[4], payload_bytes)
redis.call('XDEL', KEYS[1], ARGV[1])
local remaining = tonumber(redis.call('GET', KEYS[2]) or '0') - tonumber(fields['retained_bytes'])
if remaining > 0 then redis.call('SET', KEYS[2], remaining) else redis.call('DEL', KEYS[2]) end
redis.call('HINCRBY', KEYS[5], 'replayed', 1)
redis.call('XADD', KEYS[6], 'MAXLEN', '~', 10000, '*', 'type', 'log-ingest')
return id
)lua"};

inline constexpr std::string_view statistics_script{R"lua(
local pending = redis.call('XPENDING', KEYS[1], ARGV[1])
return {redis.call('XLEN', KEYS[1]), tonumber(redis.call('GET', KEYS[2]) or '0'), pending[1],
    redis.call('ZCARD', KEYS[3]), redis.call('XLEN', KEYS[4]),
    tonumber(redis.call('GET', KEYS[5]) or '0'),
    tonumber(redis.call('HGET', KEYS[6], 'retried') or '0'),
    tonumber(redis.call('HGET', KEYS[6], 'failed') or '0'),
    tonumber(redis.call('HGET', KEYS[6], 'finished') or '0'),
    tonumber(redis.call('HGET', KEYS[6], 'replayed') or '0')}
)lua"};

inline constexpr std::string_view reclaim_script{R"lua(
local pending = redis.call('XPENDING', KEYS[1], ARGV[1], 'IDLE', ARGV[3], '-', '+', ARGV[4])
local entries = {}
for _, entry in ipairs(pending) do
    local claimed = redis.call('XCLAIM', KEYS[1], ARGV[1], ARGV[2], ARGV[3], entry[1])
    if #claimed > 0 then
        redis.call('ZREM', KEYS[2], entry[1])
        entries[#entries + 1] = claimed[1]
    end
end
return entries
)lua"};

struct entry final {
    std::string id_;
    std::string payload_;
    bool valid_{};
};

enum class failure_result {
    archived,
    full,
    ownership_lost,
};

struct failed_entry final {
    std::string id_;
    std::string source_id_;
    std::string payload_;
    std::string reason_;
    std::int64_t attempts_{};
    std::int64_t failed_unix_ms_{};
};

struct statistics final {
    std::int64_t retained_entries_{};
    std::int64_t retained_bytes_{};
    std::int64_t pending_entries_{};
    std::int64_t scheduled_retries_{};
    std::int64_t failed_entries_{};
    std::int64_t failed_bytes_{};
    std::int64_t retries_total_{};
    std::int64_t failures_total_{};
    std::int64_t finished_total_{};
    std::int64_t replays_total_{};
};

inline std::int64_t parse_integer(std::string_view value) {
    std::int64_t parsed{};
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || parsed < 0) {
        throw std::runtime_error("invalid Redis log delivery integer");
    }
    return parsed;
}

inline entry parse_entry(const ruvia::redis_value& source) {
    if (source.kind() != ruvia::redis_value::kind_type::array) {
        throw std::runtime_error("unexpected Redis log stream entry");
    }
    const auto parts = source.array();
    if (parts.size() != 2 || parts[0].kind() != ruvia::redis_value::kind_type::string ||
        parts[1].kind() != ruvia::redis_value::kind_type::array) {
        throw std::runtime_error("unexpected Redis log stream entry");
    }
    entry entry{.id_ = std::string(parts[0].string()), .payload_ = {}, .valid_ = false};
    const auto fields = parts[1].array();
    for (std::size_t index = 0; index + 1 < fields.size(); index += 2) {
        if (fields[index].kind() == ruvia::redis_value::kind_type::string &&
            fields[index].string() == payload_field &&
            fields[index + 1].kind() == ruvia::redis_value::kind_type::string) {
            entry.payload_ = std::string(fields[index + 1].string());
            entry.valid_ = fields.size() == 2 && !entry.payload_.empty();
        }
    }
    return entry;
}

inline ruvia::task<void> initialize(ruvia::redis_handle redis) {
    const std::array<std::string_view, 1> keys{stream_key};
    const std::array<std::string_view, 1> arguments{consumer_group};
    const auto result = co_await redis.eval(initialize_script, keys, arguments);
    require_redis_success(result, "could not initialize Redis log stream");
    if (result.kind() != ruvia::redis_value::kind_type::integer ||
        (result.integer() != 0 && result.integer() != 1)) {
        throw std::runtime_error("unexpected Redis log stream initialization reply");
    }
}

inline ruvia::task<bool> push(ruvia::redis_handle redis, std::string_view payload) {
    const auto maximum_entries = std::to_string(max_entries);
    const auto maximum_bytes = std::to_string(max_retained_bytes);
    const std::array<std::string_view, 3> keys{stream_key, retained_bytes_key, service::background::work_available::stream_key};
    const std::array<std::string_view, 3> arguments{payload, maximum_entries, maximum_bytes};
    const auto result = co_await redis.eval(push_script, keys, arguments);
    require_redis_success(result, "could not append Redis log stream entry");
    if (result.kind() == ruvia::redis_value::kind_type::integer && result.integer() == 0) {
        co_return false;
    }
    if (result.kind() != ruvia::redis_value::kind_type::string || result.string().empty()) {
        throw std::runtime_error("unexpected Redis log stream append reply");
    }
    co_return true;
}

inline std::vector<entry> entries(const std::optional<ruvia::redis_x_read_group_result>& result) {
    std::vector<entry> output;
    if (!result) {
        return output;
    }
    if (result->streams().size() != 1 || result->streams().front().stream() != stream_key) {
        throw std::runtime_error("unexpected Redis log stream read reply");
    }
    const auto& stream = result->streams().front();
    output.reserve(stream.entries().size());
    for (const auto& source : stream.entries()) {
        entry entry{.id_ = std::string(source.id()), .payload_ = {}, .valid_ = false};
        for (const auto& field : source.fields()) {
            if (field.key() == payload_field) {
                entry.payload_ = std::string(field.value());
                entry.valid_ = source.fields().size() == 1 && !entry.payload_.empty();
            }
        }
        output.push_back(std::move(entry));
    }
    return output;
}

inline ruvia::task<std::vector<entry>> read(ruvia::redis_handle redis, std::string_view consumer) {
    const std::array streams{
        ruvia::redis_stream_read_view{.stream_ = stream_key, .id_ = std::string_view{">"}}};
    ruvia::redis_x_read_group_options options{.count_ = read_batch_size};
    co_return entries(co_await redis.xread_group(consumer_group, consumer, streams, options));
}

inline ruvia::task<std::chrono::milliseconds> retry_wait(ruvia::redis_handle redis,
    std::chrono::milliseconds maximum) {
    constexpr std::string_view script{R"lua(
local due = redis.call('ZRANGE', KEYS[1], 0, 0, 'WITHSCORES')
if #due == 0 then return tonumber(ARGV[1]) end
local clock = redis.call('TIME')
local now = tonumber(clock[1])*1000 + math.floor(tonumber(clock[2])/1000)
return math.max(0, math.min(tonumber(ARGV[1]), tonumber(due[2])-now))
)lua"};
    const std::array keys{retry_schedule_key};
    const auto limit = std::to_string(std::max<std::int64_t>(0, maximum.count()));
    const std::array<std::string_view, 1> arguments{limit};
    const auto result = co_await redis.eval(script, keys, arguments);
    require_redis_success(result, "could not inspect log retry deadline");
    co_return std::chrono::milliseconds(result.integer());
}

inline ruvia::task<void> finish(ruvia::redis_handle redis, std::string_view consumer,
    const entry& entry) {
    const std::array<std::string_view, 6> keys{stream_key, retained_bytes_key,
        retry_schedule_key, retry_attempts_key, statistics_key, service::background::work_available::stream_key};
    const std::array<std::string_view, 3> arguments{consumer_group, entry.id_, consumer};
    const auto result = co_await redis.eval(finish_script, keys, arguments);
    require_redis_success(result, "could not finish Redis log stream entry");
    if (result.kind() != ruvia::redis_value::kind_type::integer || result.integer() != 1) {
        throw std::runtime_error("Redis log stream entry is no longer owned by this consumer");
    }
}

inline ruvia::task<std::vector<entry>> reclaim_stale(ruvia::redis_handle redis, std::string_view consumer) {
    const auto minimum_idle = std::to_string(reclaim_idle_ms);
    const auto count = std::to_string(reclaim_batch_size);
    const std::array<std::string_view, 2> keys{stream_key, retry_schedule_key};
    const std::array<std::string_view, 4> arguments{consumer_group, consumer, minimum_idle, count};
    const auto value = co_await redis.eval(reclaim_script, keys, arguments);
    require_redis_success(value, "could not reclaim Redis log stream entries");
    if (value.kind() != ruvia::redis_value::kind_type::array) {
        throw std::runtime_error("unexpected Redis log stream reclaim reply");
    }
    std::vector<entry> entries;
    entries.reserve(value.array().size());
    for (const auto& source : value.array()) {
        entries.push_back(parse_entry(source));
    }
    co_return entries;
}

// Returns zero only after ownership was lost. Retry state survives process loss;
// temporary database failures never become an acknowledgement or a deletion.
inline ruvia::task<std::chrono::milliseconds> schedule_retry(ruvia::redis_handle redis,
    std::string_view consumer, const entry& entry) {
    const auto initial = std::to_string(retry_initial_ms);
    const auto maximum = std::to_string(retry_maximum_ms);
    const std::array<std::string_view, 4> keys{stream_key, retry_schedule_key,
        retry_attempts_key, statistics_key};
    const std::array<std::string_view, 5> arguments{consumer_group, entry.id_, consumer, initial, maximum};
    const auto result = co_await redis.eval(retry_script, keys, arguments);
    require_redis_success(result, "could not schedule Redis log delivery retry");
    if (result.kind() != ruvia::redis_value::kind_type::integer || result.integer() < 0 ||
        result.integer() > retry_maximum_ms) {
        throw std::runtime_error("unexpected Redis log delivery retry reply");
    }
    co_return std::chrono::milliseconds(result.integer());
}

inline ruvia::task<std::vector<entry>> claim_retries(ruvia::redis_handle redis, std::string_view consumer) {
    const auto count = std::to_string(read_batch_size);
    const std::array<std::string_view, 3> keys{stream_key, retry_schedule_key, retry_attempts_key};
    const std::array<std::string_view, 3> arguments{consumer_group, consumer, count};
    const auto result = co_await redis.eval(claim_retry_script, keys, arguments);
    require_redis_success(result, "could not claim Redis log delivery retries");
    if (result.kind() != ruvia::redis_value::kind_type::array) {
        throw std::runtime_error("unexpected Redis log delivery retry claim reply");
    }
    std::vector<entry> output;
    output.reserve(result.array().size());
    for (const auto& source : result.array()) {
        output.push_back(parse_entry(source));
    }
    co_return output;
}

// A full failure archive applies backpressure: the input remains pending and
// its caller schedules another retry. No oldest-entry trimming is used here.
inline ruvia::task<failure_result> fail(ruvia::redis_handle redis, std::string_view consumer,
    const entry& entry, std::string_view reason) {
    if (reason.empty() || reason.size() > 256) {
        throw std::invalid_argument("log failure reason must contain at most 256 bytes");
    }
    const auto maximum_entries = std::to_string(max_failed_entries);
    const auto maximum_bytes = std::to_string(max_failed_bytes);
    const std::array<std::string_view, 7> keys{stream_key, retained_bytes_key,
        retry_schedule_key, retry_attempts_key, failed_stream_key, failed_bytes_key, statistics_key};
    const std::array<std::string_view, 6> arguments{consumer_group, entry.id_, consumer,
        reason, maximum_entries, maximum_bytes};
    const auto result = co_await redis.eval(fail_script, keys, arguments);
    require_redis_success(result, "could not archive failed Redis log delivery");
    if (result.kind() != ruvia::redis_value::kind_type::integer) {
        throw std::runtime_error("unexpected Redis log failure archive reply");
    }
    switch (result.integer()) {
        case 1:
            co_return failure_result::archived;
        case 0:
            co_return failure_result::full;
        case -1:
            co_return failure_result::ownership_lost;
        default:
            throw std::runtime_error("unexpected Redis log failure archive result");
    }
}

inline ruvia::task<std::vector<failed_entry>> read_failed(ruvia::redis_handle redis,
    std::string_view after = "-", std::size_t count = 100) {
    if (count == 0 || count > static_cast<std::size_t>(max_failed_entries)) {
        throw std::invalid_argument("invalid failed log delivery read size");
    }
    const auto limit = std::to_string(count);
    const auto start = after == "-" ? std::string(after) : "(" + std::string(after);
    const auto result = co_await redis.command("XRANGE", failed_stream_key, start, "+", "COUNT", limit);
    require_redis_success(result, "could not read failed Redis log deliveries");
    if (result.kind() != ruvia::redis_value::kind_type::array) {
        throw std::runtime_error("unexpected Redis failed log delivery read reply");
    }
    std::vector<failed_entry> output;
    output.reserve(result.array().size());
    for (const auto& source : result.array()) {
        const auto parts = source.array();
        if (parts.size() != 2 || parts[0].kind() != ruvia::redis_value::kind_type::string ||
            parts[1].kind() != ruvia::redis_value::kind_type::array) {
            throw std::runtime_error("invalid Redis failed log delivery");
        }
        failed_entry entry{};
        entry.id_ = parts[0].string();
        const auto fields = parts[1].array();
        if (fields.size() != 12) {
            throw std::runtime_error("invalid Redis failed log delivery fields");
        }
        for (std::size_t index = 0; index < fields.size(); index += 2) {
            if (fields[index].kind() != ruvia::redis_value::kind_type::string ||
                fields[index + 1].kind() != ruvia::redis_value::kind_type::string) {
                throw std::runtime_error("invalid Redis failed log delivery field");
            }
            const auto name = fields[index].string();
            const auto value = fields[index + 1].string();
            if (name == "source_id") {
                entry.source_id_ = value;
            } else if (name == "payload") {
                entry.payload_ = value;
            } else if (name == "reason") {
                entry.reason_ = value;
            } else if (name == "attempts") {
                entry.attempts_ = parse_integer(value);
            } else if (name == "failed_unix_ms") {
                entry.failed_unix_ms_ = parse_integer(value);
            }
        }
        output.push_back(std::move(entry));
    }
    co_return output;
}

// Empty means the record is absent or the live queue has reached its budget.
// The original failed record is retained until the live append succeeds.
inline ruvia::task<std::optional<std::string>> replay_failed(ruvia::redis_handle redis,
    std::string_view failed_id) {
    const auto maximum_entries = std::to_string(max_entries);
    const auto maximum_bytes = std::to_string(max_retained_bytes);
    const std::array<std::string_view, 6> keys{failed_stream_key, failed_bytes_key,
        stream_key, retained_bytes_key, statistics_key, service::background::work_available::stream_key};
    const std::array<std::string_view, 3> arguments{failed_id, maximum_entries, maximum_bytes};
    const auto result = co_await redis.eval(replay_failed_script, keys, arguments);
    require_redis_success(result, "could not replay failed Redis log delivery");
    if (result.kind() == ruvia::redis_value::kind_type::integer && result.integer() == 0) {
        co_return std::nullopt;
    }
    if (result.kind() != ruvia::redis_value::kind_type::string || result.string().empty()) {
        throw std::runtime_error("unexpected Redis log delivery replay reply");
    }
    co_return std::string(result.string());
}

inline ruvia::task<statistics> inspect_statistics(ruvia::redis_handle redis) {
    const std::array<std::string_view, 6> keys{stream_key, retained_bytes_key,
        retry_schedule_key, failed_stream_key, failed_bytes_key, statistics_key};
    const std::array<std::string_view, 1> arguments{consumer_group};
    const auto result = co_await redis.eval(statistics_script, keys, arguments);
    require_redis_success(result, "could not inspect Redis log delivery statistics");
    if (result.kind() != ruvia::redis_value::kind_type::array || result.array().size() != 10) {
        throw std::runtime_error("unexpected Redis log delivery statistics reply");
    }
    std::array<std::int64_t, 10> values{};
    for (std::size_t index = 0; index < values.size(); ++index) {
        const auto& value = result.array()[index];
        if (value.kind() != ruvia::redis_value::kind_type::integer || value.integer() < 0) {
            throw std::runtime_error("invalid Redis log delivery statistic");
        }
        values[index] = value.integer();
    }
    co_return statistics{.retained_entries_ = values[0], .retained_bytes_ = values[1], .pending_entries_ = values[2], .scheduled_retries_ = values[3], .failed_entries_ = values[4], .failed_bytes_ = values[5], .retries_total_ = values[6], .failures_total_ = values[7], .finished_total_ = values[8], .replays_total_ = values[9]};
}

}  // namespace service::log_ingest::queue
