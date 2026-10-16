#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/redis/redis.h>

#include <algorithm>
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

#include "service/utils/redis_reply.h"

// A durable Redis Streams work queue with a consumer group. Producers append
// within a byte budget; claimants own an entry until they remove it, schedule
// a retry (exponential backoff kept in a retry ZSET and an attempts HASH) or
// archive it to a bounded failure stream. Entries of a claimant that stopped
// are reclaimed after an idle period, so no work depends on one Worker.
// Queues keep their own entry formats and completion side effects; they
// compose their scripts from the Lua primitives below.
namespace service::background::reliable_stream {

// Keys, group and limits of one queue. Key names and the consumer group are
// persistent Redis state: they never change across releases.
struct definition final {
    std::string_view stream_;
    std::string_view group_;
    // STRING: bytes the producer budget accounts to retained live entries.
    std::string_view bytes_;
    // ZSET: entry id -> Unix milliseconds at which its retry is due.
    std::string_view retry_;
    // HASH: entry id -> attempts so far.
    std::string_view attempts_;
    std::string_view failed_;
    std::string_view failed_bytes_;
    std::int64_t max_failed_entries_{};
    std::int64_t max_failed_bytes_{};
    std::chrono::milliseconds retry_initial_{};
    std::chrono::milliseconds retry_maximum_{};
    std::chrono::milliseconds reclaim_idle_{};
};

// Delay before retry `attempt` (1-based): doubles from `initial`, capped at
// `maximum`. `reliable_backoff` below is the same function inside Redis.
[[nodiscard]] constexpr std::chrono::milliseconds backoff(std::int64_t attempt, std::chrono::milliseconds initial,
    std::chrono::milliseconds maximum) noexcept {
    const auto doublings = std::clamp<std::int64_t>(attempt - 1, 0, 16);
    return std::min(maximum, initial * (std::int64_t{1} << doublings));
}

// Lua primitives shared by every reliable stream script. Callers validate
// key types before their first mutation: Lua errors do not roll back.
inline constexpr std::string_view lua{R"lua(
local function reliable_now()
    local clock = redis.call('TIME')
    return tonumber(clock[1]) * 1000 + math.floor(tonumber(clock[2]) / 1000)
end
local function reliable_owned(stream, group, id, consumer)
    local pending = redis.call('XPENDING', stream, group, id, id, 1)
    return #pending == 1 and pending[1][2] == consumer
end
local function reliable_fields(stream, id)
    local rows = redis.call('XRANGE', stream, id, id, 'COUNT', 1)
    if #rows == 0 then return nil end
    local fields = {}
    for index = 1, #rows[1][2], 2 do fields[rows[1][2][index]] = rows[1][2][index + 1] end
    return fields
end
local function reliable_counter(key)
    local text = redis.call('GET', key) or '0'
    if not string.match(text, '^%d+$') then error('invalid reliable stream byte counter') end
    return tonumber(text)
end
local function reliable_admit(bytes, count, maximum_entries, maximum_bytes, retained)
    return count < maximum_entries and retained <= maximum_bytes - reliable_counter(bytes)
end
local function reliable_release(bytes, retained)
    local remaining = reliable_counter(bytes) - retained
    if remaining > 0 then redis.call('SET', bytes, remaining) else redis.call('DEL', bytes) end
end
local function reliable_backoff(attempt, initial, maximum)
    return math.min(maximum, initial * 2 ^ math.min(attempt - 1, 16))
end
local function reliable_retry(stream, group, retry, attempts, id, consumer, initial, maximum)
    local delay = reliable_backoff(redis.call('HINCRBY', attempts, id, 1), initial, maximum)
    redis.call('ZADD', retry, reliable_now() + delay, id)
    redis.call('XCLAIM', stream, group, consumer, 0, id, 'JUSTID')
    return delay
end
local function reliable_remove(stream, group, retry, attempts, bytes, id, retained)
    redis.call('XACK', stream, group, id)
    redis.call('XDEL', stream, id)
    reliable_release(bytes, retained)
    redis.call('ZREM', retry, id)
    redis.call('HDEL', attempts, id)
end
local function reliable_archive(failed, failed_bytes, maximum_entries, maximum_bytes, record_bytes, fields)
    if redis.call('XLEN', failed) >= maximum_entries or
       record_bytes > maximum_bytes - reliable_counter(failed_bytes) then
        return false
    end
    redis.call('XADD', failed, '*', unpack(fields))
    redis.call('INCRBY', failed_bytes, record_bytes)
    return true
end
)lua"};

namespace detail {

template <const std::string_view& body>
inline constexpr auto script_storage = [] {
    std::array<char, lua.size() + body.size()> output{};
    std::copy(lua.begin(), lua.end(), output.begin());
    std::copy(body.begin(), body.end(), output.begin() + lua.size());
    return output;
}();

}  // namespace detail

// `body` prefixed with the primitives, assembled at compile time.
template <const std::string_view& body>
inline constexpr std::string_view script{detail::script_storage<body>.data(), detail::script_storage<body>.size()};

inline constexpr std::string_view initialize_body{R"lua(
local result = redis.pcall('XGROUP', 'CREATE', KEYS[1], ARGV[1], '0', 'MKSTREAM')
if type(result) == 'table' and result.err and not string.find(result.err, 'BUSYGROUP', 1, true) then
    return redis.error_reply(result.err)
end
return 1
)lua"};

// Claims due retries; a retry whose entry is gone drops its attempts.
inline constexpr std::string_view claim_due_body{R"lua(
local ids = redis.call('ZRANGEBYSCORE', KEYS[2], '-inf', reliable_now(), 'LIMIT', 0, ARGV[3])
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

// Claims entries whose claimant has been idle for ARGV[3] milliseconds.
inline constexpr std::string_view reclaim_body{R"lua(
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

inline constexpr std::string_view next_due_body{R"lua(
local due = redis.call('ZRANGE', KEYS[1], 0, 0, 'WITHSCORES')
if #due == 0 then return tonumber(ARGV[1]) end
return math.max(0, math.min(tonumber(ARGV[1]), tonumber(due[2]) - reliable_now()))
)lua"};

// One claimed entry with its field/value pairs in stream order.
struct entry final {
    std::string id_;
    std::vector<std::pair<std::string, std::string>> fields_;

    [[nodiscard]] std::optional<std::string_view> field(std::string_view name) const {
        for (const auto& [key, value] : fields_) {
            if (key == name) {
                return std::string_view{value};
            }
        }
        return std::nullopt;
    }
};

inline entry parse_entry(const ruvia::redis_value& source) {
    if (source.kind() != ruvia::redis_value::kind_type::array || source.array().size() != 2) {
        throw std::runtime_error("unexpected reliable stream entry");
    }
    const auto parts = source.array();
    if (parts[0].kind() != ruvia::redis_value::kind_type::string ||
        parts[1].kind() != ruvia::redis_value::kind_type::array || parts[1].array().size() % 2 != 0) {
        throw std::runtime_error("unexpected reliable stream entry");
    }
    entry output{.id_ = std::string(parts[0].string()), .fields_ = {}};
    const auto fields = parts[1].array();
    output.fields_.reserve(fields.size() / 2);
    for (std::size_t index = 0; index < fields.size(); index += 2) {
        if (fields[index].kind() != ruvia::redis_value::kind_type::string ||
            fields[index + 1].kind() != ruvia::redis_value::kind_type::string) {
            throw std::runtime_error("unexpected reliable stream field");
        }
        output.fields_.emplace_back(fields[index].string(), fields[index + 1].string());
    }
    return output;
}

inline std::vector<entry> parse_entries(const ruvia::redis_value& value, std::string_view operation) {
    service::utils::require_redis_success(value, std::string("could not ") + std::string(operation));
    if (value.kind() != ruvia::redis_value::kind_type::array) {
        throw std::runtime_error("unexpected reply to " + std::string(operation));
    }
    std::vector<entry> output;
    output.reserve(value.array().size());
    for (const auto& source : value.array()) {
        output.push_back(parse_entry(source));
    }
    return output;
}

inline ruvia::task<void> initialize(ruvia::redis_handle redis, const definition& queue) {
    const std::array keys{queue.stream_};
    const std::array arguments{queue.group_};
    const auto result = co_await redis.eval(script<initialize_body>, keys, arguments);
    service::utils::require_redis_success(result, "could not initialize reliable stream group");
    if (result.kind() != ruvia::redis_value::kind_type::integer || result.integer() != 1) {
        throw std::runtime_error("unexpected reliable stream initialization reply");
    }
}

// Up to `count` entries never delivered to any claimant of the group.
inline ruvia::task<std::vector<entry>> read(ruvia::redis_handle redis, const definition& queue,
    std::string_view consumer, std::uint64_t count) {
    const std::array streams{ruvia::redis_stream_read_view{.stream_ = queue.stream_, .id_ = std::string_view{">"}}};
    const auto result = co_await redis.xread_group(queue.group_, consumer, streams, {.count_ = count});
    std::vector<entry> output;
    if (!result) {
        co_return output;
    }
    if (result->streams().size() != 1 || result->streams().front().stream() != queue.stream_) {
        throw std::runtime_error("unexpected reliable stream read reply");
    }
    const auto& stream = result->streams().front();
    output.reserve(stream.entries().size());
    for (const auto& source : stream.entries()) {
        entry value{.id_ = std::string(source.id()), .fields_ = {}};
        value.fields_.reserve(source.fields().size());
        for (const auto& field : source.fields()) {
            value.fields_.emplace_back(field.key(), field.value());
        }
        output.push_back(std::move(value));
    }
    co_return output;
}

// Up to `count` retries whose backoff has elapsed, now owned by `consumer`.
inline ruvia::task<std::vector<entry>> claim_due(ruvia::redis_handle redis, const definition& queue,
    std::string_view consumer, std::uint64_t count) {
    const std::array keys{queue.stream_, queue.retry_, queue.attempts_};
    const auto limit = std::to_string(count);
    const std::array<std::string_view, 3> arguments{queue.group_, consumer, limit};
    co_return parse_entries(co_await redis.eval(script<claim_due_body>, keys, arguments), "claim due stream retries");
}

// Up to `count` entries abandoned by a claimant that stopped (idle for at
// least `reclaim_idle_`), now owned by `consumer`.
inline ruvia::task<std::vector<entry>> reclaim(ruvia::redis_handle redis, const definition& queue,
    std::string_view consumer, std::uint64_t count) {
    const std::array keys{queue.stream_, queue.retry_};
    const auto idle = std::to_string(queue.reclaim_idle_.count());
    const auto limit = std::to_string(count);
    const std::array<std::string_view, 4> arguments{queue.group_, consumer, idle, limit};
    co_return parse_entries(co_await redis.eval(script<reclaim_body>, keys, arguments), "reclaim abandoned stream entries");
}

// Time until the earliest scheduled retry is due, at most `maximum`.
inline ruvia::task<std::chrono::milliseconds> next_due(ruvia::redis_handle redis, const definition& queue,
    std::chrono::milliseconds maximum) {
    const std::array keys{queue.retry_};
    const auto limit = std::to_string(std::max<std::int64_t>(0, maximum.count()));
    const std::array<std::string_view, 1> arguments{limit};
    const auto result = co_await redis.eval(script<next_due_body>, keys, arguments);
    service::utils::require_redis_success(result, "could not inspect stream retry deadline");
    if (result.kind() != ruvia::redis_value::kind_type::integer || result.integer() < 0) {
        throw std::runtime_error("unexpected stream retry deadline reply");
    }
    co_return std::chrono::milliseconds(result.integer());
}

}  // namespace service::background::reliable_stream
