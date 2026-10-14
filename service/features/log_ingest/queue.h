#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/redis/redis.h>

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

#include "service/features/log_ingest/redis_protocol.h"

namespace service::log_ingest::queue {

inline constexpr std::string_view stream_key{"flexedge:log-deliveries:v3"};
inline constexpr std::string_view retained_bytes_key{"flexedge:log-deliveries:v3:bytes"};
inline constexpr std::string_view consumer_group{"flexedge-log-ingest-v3"};
inline constexpr std::string_view payload_field{"payload"};
inline constexpr std::int64_t max_entries{200000};
inline constexpr std::int64_t max_retained_bytes{512 * 1024 * 1024};
inline constexpr std::int64_t max_delivery_attempts{4};
inline constexpr std::uint64_t read_batch_size{32};
inline constexpr std::uint64_t reclaim_batch_size{32};
inline constexpr std::int64_t reclaim_idle_ms{5000};
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
inline constexpr std::string_view finish_script{R"lua(
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

struct entry final {
    std::string id_;
    std::string payload_;
    bool valid_{};
};

struct auto_claim_result final {
    std::string next_cursor_;
    std::vector<entry> entries_;
};

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
    if (fields.size() == 2 && fields[0].kind() == ruvia::redis_value::kind_type::string &&
        fields[0].string() == payload_field &&
        fields[1].kind() == ruvia::redis_value::kind_type::string) {
        entry.payload_ = std::string(fields[1].string());
        entry.valid_ = !entry.payload_.empty();
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
    const std::array<std::string_view, 2> keys{stream_key, retained_bytes_key};
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
        if (source.fields().size() == 1 && source.fields().front().key() == payload_field) {
            entry.payload_ = std::string(source.fields().front().value());
            entry.valid_ = !entry.payload_.empty();
        }
        output.push_back(std::move(entry));
    }
    return output;
}

inline ruvia::task<std::vector<entry>> read(ruvia::redis_handle redis, std::string_view consumer) {
    const std::array streams{
        ruvia::redis_stream_read_view{.stream_ = stream_key, .id_ = std::string_view{">"}}};
    ruvia::redis_x_read_group_options options{.count_ = read_batch_size};
    options.block_ = ruvia::redis_block_wait::for_duration(std::chrono::seconds(1));
    co_return entries(co_await redis.xread_group(consumer_group, consumer, streams, options));
}

inline ruvia::task<void> finish(ruvia::redis_handle redis, std::string_view consumer,
    const entry& entry) {
    const auto payload_bytes = std::to_string(entry.payload_.size());
    const std::array<std::string_view, 2> keys{stream_key, retained_bytes_key};
    const std::array<std::string_view, 4> arguments{consumer_group, entry.id_, payload_bytes,
        consumer};
    const auto result = co_await redis.eval(finish_script, keys, arguments);
    require_redis_success(result, "could not finish Redis log stream entry");
    if (result.kind() != ruvia::redis_value::kind_type::integer || result.integer() != 1) {
        throw std::runtime_error("Redis log stream entry is no longer owned by this consumer");
    }
}

inline ruvia::task<auto_claim_result> auto_claim(ruvia::redis_handle redis, std::string_view consumer,
    std::string_view cursor) {
    const auto minimum_idle = std::to_string(reclaim_idle_ms);
    const auto count = std::to_string(reclaim_batch_size);
    const auto value = co_await redis.command("XAUTOCLAIM", stream_key, consumer_group, consumer,
        minimum_idle, cursor, "COUNT", count);
    require_redis_success(value, "could not reclaim Redis log stream entries");
    if (value.kind() != ruvia::redis_value::kind_type::array) {
        throw std::runtime_error("unexpected Redis log stream reclaim reply");
    }
    const auto parts = value.array();
    if ((parts.size() != 2 && parts.size() != 3) ||
        parts[0].kind() != ruvia::redis_value::kind_type::string ||
        parts[1].kind() != ruvia::redis_value::kind_type::array ||
        (parts.size() == 3 && parts[2].kind() != ruvia::redis_value::kind_type::array)) {
        throw std::runtime_error("unexpected Redis log stream reclaim reply");
    }
    std::vector<entry> entries;
    entries.reserve(parts[1].array().size());
    for (const auto& source : parts[1].array()) {
        entries.push_back(parse_entry(source));
    }
    co_return auto_claim_result{.next_cursor_ = std::string(parts[0].string()),
        .entries_ = std::move(entries)};
}

inline ruvia::task<std::optional<std::int64_t>>
delivery_count(ruvia::redis_handle redis, std::string_view consumer, std::string_view entry_id) {
    const auto value = co_await redis.command("XPENDING", stream_key, consumer_group, entry_id,
        entry_id, "1", consumer);
    require_redis_success(value, "could not inspect Redis log stream delivery count");
    if (value.kind() != ruvia::redis_value::kind_type::array) {
        throw std::runtime_error("unexpected Redis log stream delivery count reply");
    }
    const auto rows = value.array();
    if (rows.empty()) {
        co_return std::nullopt;
    }
    if (rows.size() != 1 || rows.front().kind() != ruvia::redis_value::kind_type::array) {
        throw std::runtime_error("unexpected Redis log stream delivery count reply");
    }
    const auto row = rows.front().array();
    if (row.size() != 4 || row[0].kind() != ruvia::redis_value::kind_type::string ||
        row[0].string() != entry_id || row[3].kind() != ruvia::redis_value::kind_type::integer) {
        throw std::runtime_error("unexpected Redis log stream delivery count reply");
    }
    co_return row[3].integer();
}

}  // namespace service::log_ingest::queue
