#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/redis/redis.h>

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

#include "service/features/background/reliable_stream.h"
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

inline constexpr service::background::reliable_stream::definition stream{
    .stream_ = stream_key,
    .group_ = consumer_group,
    .bytes_ = retained_bytes_key,
    .retry_ = retry_schedule_key,
    .attempts_ = retry_attempts_key,
    .failed_ = failed_stream_key,
    .failed_bytes_ = failed_bytes_key,
    .max_failed_entries_ = max_failed_entries,
    .max_failed_bytes_ = max_failed_bytes,
    .retry_initial_ = std::chrono::milliseconds(retry_initial_ms),
    .retry_maximum_ = std::chrono::milliseconds(retry_maximum_ms),
    .reclaim_idle_ = std::chrono::milliseconds(reclaim_idle_ms),
};

// The live budget accounts payload bytes only.
inline constexpr std::string_view push_body{R"lua(
local payload_bytes = string.len(ARGV[1])
if not reliable_admit(KEYS[2], redis.call('XLEN', KEYS[1]), tonumber(ARGV[2]), tonumber(ARGV[3]), payload_bytes) then
    return 0
end
local id = redis.call('XADD', KEYS[1], '*', 'payload', ARGV[1])
redis.call('INCRBY', KEYS[2], payload_bytes)
return id
)lua"};

inline constexpr std::string_view finish_body{R"lua(
local kinds = {'stream','string','zset','hash','hash'}
for index, kind in ipairs(kinds) do
    local actual = redis.call('TYPE', KEYS[index]).ok
    if actual ~= 'none' and actual ~= kind then return redis.error_reply('invalid log completion key type') end
end
local pending = redis.call('XPENDING', KEYS[1], ARGV[1], ARGV[2], ARGV[2], '1')
if #pending > 0 and (#pending[1] < 2 or pending[1][2] ~= ARGV[3]) then
    return 0
end
local fields = reliable_fields(KEYS[1], ARGV[2])
if fields == nil then return 1 end
if #pending == 0 then return 0 end
reliable_remove(KEYS[1], ARGV[1], KEYS[3], KEYS[4], KEYS[2], ARGV[2], string.len(fields['payload'] or ''))
redis.call('HINCRBY', KEYS[5], 'finished', 1)
return 1
)lua"};

inline constexpr std::string_view retry_body{R"lua(
if not reliable_owned(KEYS[1], ARGV[1], ARGV[2], ARGV[3]) then return 0 end
local delay = reliable_retry(KEYS[1], ARGV[1], KEYS[2], KEYS[3], ARGV[2], ARGV[3], tonumber(ARGV[4]), tonumber(ARGV[5]))
redis.call('HINCRBY', KEYS[4], 'retried', 1)
return delay
)lua"};

inline constexpr std::string_view fail_body{R"lua(
if not reliable_owned(KEYS[1], ARGV[1], ARGV[2], ARGV[3]) then return -1 end
local fields = reliable_fields(KEYS[1], ARGV[2])
if fields == nil then return -1 end
local payload = fields['payload'] or ''
local reason = ARGV[4]
local record_bytes = string.len(payload) + string.len(reason) + string.len(ARGV[2]) + 64
local attempts = redis.call('HGET', KEYS[4], ARGV[2]) or '0'
local archived = reliable_archive(KEYS[5], KEYS[6], tonumber(ARGV[5]), tonumber(ARGV[6]), record_bytes,
    {'source_id', ARGV[2], 'payload', payload, 'reason', reason, 'attempts', attempts,
     'failed_unix_ms', tostring(reliable_now()), 'retained_bytes', tostring(record_bytes)})
if not archived then return 0 end
reliable_remove(KEYS[1], ARGV[1], KEYS[3], KEYS[4], KEYS[2], ARGV[2], string.len(payload))
redis.call('HINCRBY', KEYS[7], 'failed', 1)
return 1
)lua"};

inline constexpr std::string_view replay_failed_body{R"lua(
local fields = reliable_fields(KEYS[1], ARGV[1])
if fields == nil then return 0 end
if fields['payload'] == nil or fields['retained_bytes'] == nil then
    return redis.error_reply('invalid failed log delivery')
end
local payload_bytes = string.len(fields['payload'])
if not reliable_admit(KEYS[4], redis.call('XLEN', KEYS[3]), tonumber(ARGV[2]), tonumber(ARGV[3]), payload_bytes) then
    return 0
end
local id = redis.call('XADD', KEYS[3], '*', 'payload', fields['payload'])
redis.call('INCRBY', KEYS[4], payload_bytes)
redis.call('XDEL', KEYS[1], ARGV[1])
reliable_release(KEYS[2], tonumber(fields['retained_bytes']))
redis.call('HINCRBY', KEYS[5], 'replayed', 1)
return id
)lua"};

inline constexpr std::string_view statistics_body{R"lua(
local pending = redis.call('XPENDING', KEYS[1], ARGV[1])
return {redis.call('XLEN', KEYS[1]), reliable_counter(KEYS[2]), pending[1],
    redis.call('ZCARD', KEYS[3]), redis.call('XLEN', KEYS[4]),
    reliable_counter(KEYS[5]),
    tonumber(redis.call('HGET', KEYS[6], 'retried') or '0'),
    tonumber(redis.call('HGET', KEYS[6], 'failed') or '0'),
    tonumber(redis.call('HGET', KEYS[6], 'finished') or '0'),
    tonumber(redis.call('HGET', KEYS[6], 'replayed') or '0')}
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

// A delivery is valid only as a single non-empty payload field.
inline entry to_entry(service::background::reliable_stream::entry source) {
    entry output{.id_ = std::move(source.id_), .payload_ = {}, .valid_ = false};
    for (auto& [name, value] : source.fields_) {
        if (name == payload_field) {
            output.payload_ = std::move(value);
            output.valid_ = source.fields_.size() == 1 && !output.payload_.empty();
        }
    }
    return output;
}

inline std::vector<entry> to_entries(std::vector<service::background::reliable_stream::entry> sources) {
    std::vector<entry> output;
    output.reserve(sources.size());
    for (auto& source : sources) {
        output.push_back(to_entry(std::move(source)));
    }
    return output;
}

inline ruvia::task<void> initialize(ruvia::redis_handle redis) {
    co_await service::background::reliable_stream::initialize(redis, stream);
}

inline ruvia::task<bool> push(ruvia::redis_handle redis, std::string_view payload) {
    const auto maximum_entries = std::to_string(max_entries);
    const auto maximum_bytes = std::to_string(max_retained_bytes);
    const std::array<std::string_view, 2> keys{stream_key, retained_bytes_key};
    const std::array<std::string_view, 3> arguments{payload, maximum_entries, maximum_bytes};
    const auto result = co_await redis.eval(service::background::reliable_stream::script<push_body>, keys, arguments);
    require_redis_success(result, "could not append Redis log stream entry");
    if (result.kind() == ruvia::redis_value::kind_type::integer && result.integer() == 0) {
        co_return false;
    }
    if (result.kind() != ruvia::redis_value::kind_type::string || result.string().empty()) {
        throw std::runtime_error("unexpected Redis log stream append reply");
    }
    co_return true;
}

inline ruvia::task<std::vector<entry>> read(ruvia::redis_handle redis, std::string_view consumer) {
    co_return to_entries(co_await service::background::reliable_stream::read(redis, stream, consumer, read_batch_size));
}

inline ruvia::task<std::chrono::milliseconds> retry_wait(ruvia::redis_handle redis,
    std::chrono::milliseconds maximum) {
    co_return co_await service::background::reliable_stream::next_due(redis, stream, maximum);
}

inline ruvia::task<void> finish(ruvia::redis_handle redis, std::string_view consumer,
    const entry& entry) {
    const std::array<std::string_view, 5> keys{stream_key, retained_bytes_key,
        retry_schedule_key, retry_attempts_key, statistics_key};
    const std::array<std::string_view, 3> arguments{consumer_group, entry.id_, consumer};
    const auto result = co_await redis.eval(service::background::reliable_stream::script<finish_body>, keys, arguments);
    require_redis_success(result, "could not finish Redis log stream entry");
    if (result.kind() != ruvia::redis_value::kind_type::integer || result.integer() != 1) {
        throw std::runtime_error("Redis log stream entry is no longer owned by this consumer");
    }
}

inline ruvia::task<std::vector<entry>> reclaim_stale(ruvia::redis_handle redis, std::string_view consumer) {
    co_return to_entries(co_await service::background::reliable_stream::reclaim(redis, stream, consumer, reclaim_batch_size));
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
    const auto result = co_await redis.eval(service::background::reliable_stream::script<retry_body>, keys, arguments);
    require_redis_success(result, "could not schedule Redis log delivery retry");
    if (result.kind() != ruvia::redis_value::kind_type::integer || result.integer() < 0 ||
        result.integer() > retry_maximum_ms) {
        throw std::runtime_error("unexpected Redis log delivery retry reply");
    }
    co_return std::chrono::milliseconds(result.integer());
}

inline ruvia::task<std::vector<entry>> claim_retries(ruvia::redis_handle redis, std::string_view consumer) {
    co_return to_entries(co_await service::background::reliable_stream::claim_due(redis, stream, consumer, read_batch_size));
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
    const auto result = co_await redis.eval(service::background::reliable_stream::script<fail_body>, keys, arguments);
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
// The original failed record is retained until the live append succeeds. The
// caller wakes its own log ingest loop for the replayed delivery.
inline ruvia::task<std::optional<std::string>> replay_failed(ruvia::redis_handle redis,
    std::string_view failed_id) {
    const auto maximum_entries = std::to_string(max_entries);
    const auto maximum_bytes = std::to_string(max_retained_bytes);
    const std::array<std::string_view, 5> keys{failed_stream_key, failed_bytes_key,
        stream_key, retained_bytes_key, statistics_key};
    const std::array<std::string_view, 3> arguments{failed_id, maximum_entries, maximum_bytes};
    const auto result = co_await redis.eval(service::background::reliable_stream::script<replay_failed_body>, keys, arguments);
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
    const auto result = co_await redis.eval(service::background::reliable_stream::script<statistics_body>, keys, arguments);
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
