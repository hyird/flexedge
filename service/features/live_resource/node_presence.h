#pragma once

#include <ruvia/core/task.h>
#include <ruvia/core/timer.h>
#include <ruvia/web/db/db_repository.h>
#include <ruvia/web/redis/redis.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "service/domains/node/node_connection.h"
#include "service/features/live_resource/node_presence_entity.h"
#include "service/features/live_resource/notifications.h"
#include "service/features/live_resource/redis_snapshot_cache.h"
#include "service/features/logging/logger.h"
#include "service/utils/redis_reply.h"
#include "service/utils/token.h"

namespace service::live_resource::node_presence {

inline constexpr std::string_view deadlines_key{"flexedge:node-presence:v1:deadlines"};
inline constexpr std::string_view generation_key{"flexedge:node-presence:v1:generation"};
inline constexpr std::string_view seeded_generation_key{"flexedge:node-presence:v1:seeded-generation"};
inline constexpr auto command_timeout = std::chrono::seconds(2);
inline constexpr auto idle_wait = std::chrono::seconds(1);

// Shared by committed heartbeat publication and restart seeding. A delayed
// outbox event cannot resurrect an already-expired window, and ZADD GT never
// overwrites a more recent heartbeat. The window is the same public policy as
// node list, detail and overview queries.
inline const std::string record_lua = std::string{R"lua(
local function presence_validate(deadlines)
  local kind = redis.call('TYPE', deadlines).ok
  if kind ~= 'none' and kind ~= 'zset' then error('invalid presence deadline key type') end
end
local function presence_record(deadlines, tenant, node, seen)
  if tenant == '' or node == '' or string.find(tenant, '|', 1, true) or string.find(node, '|', 1, true) then
    error('invalid presence identity')
  end
  seen = tonumber(seen)
  if not seen or seen <= 0 then error('invalid presence timestamp') end
  local clock = redis.call('TIME')
  local now = tonumber(clock[1]) * 1000 + math.floor(tonumber(clock[2]) / 1000)
  local deadline = seen + )lua"} +
                                      std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(service::node::online_window).count()) + R"lua(
  if deadline <= now then return 0 end
  return redis.call('ZADD', deadlines, 'GT', deadline, tenant .. '|' .. node)
end
)lua";

inline ruvia::task<void> record(ruvia::redis_handle redis, std::string_view tenant,
    std::string_view node, std::int64_t seen_unix_ms) {
    const std::string script = record_lua + R"lua(
presence_validate(KEYS[1])
return presence_record(KEYS[1], ARGV[1], ARGV[2], ARGV[3])
)lua";
    const std::array keys{deadlines_key};
    const auto timestamp = std::to_string(seen_unix_ms);
    const std::array<std::string_view, 3> arguments{tenant, node, timestamp};
    service::utils::require_redis_success(co_await redis.eval(script, keys, arguments), "could not record node presence");
}

struct due_node final {
    std::string member_;
    std::string deadline_;
};

struct inspection final {
    std::string generation_;
    std::vector<due_node> due_;
    std::chrono::milliseconds wait_;
};

inline constexpr std::string_view inspect_script = R"lua(
local kind = redis.call('TYPE', KEYS[1]).ok
if kind ~= 'none' and kind ~= 'zset' then error('invalid presence deadline key type') end
redis.call('SET', KEYS[2], ARGV[1], 'NX')
local generation = redis.call('GET', KEYS[2])
local clock = redis.call('TIME')
local now = tonumber(clock[1]) * 1000 + math.floor(tonumber(clock[2]) / 1000)
local due = redis.call('ZRANGEBYSCORE', KEYS[1], '-inf', '(' .. tostring(now), 'WITHSCORES', 'LIMIT', 0, 32)
local next = redis.call('ZRANGE', KEYS[1], 0, 0, 'WITHSCORES')
local wait = 1000
if #next > 0 then wait = math.min(1000, math.max(0, tonumber(next[2]) - now + 1)) end
return {generation, due, wait}
)lua";

inline ruvia::task<inspection> inspect(ruvia::redis_handle redis) {
    const std::array keys{deadlines_key, generation_key};
    const auto nonce = service::utils::random_token();
    const std::array<std::string_view, 1> arguments{nonce};
    const auto reply = co_await redis.eval(inspect_script, keys, arguments);
    service::utils::require_redis_success(reply, "could not inspect node presence");
    const auto parts = reply.array();
    if (parts.size() != 3 || parts[0].kind() != ruvia::redis_value::kind_type::string ||
        parts[1].kind() != ruvia::redis_value::kind_type::array ||
        parts[2].kind() != ruvia::redis_value::kind_type::integer || parts[2].integer() < 0 || parts[2].integer() > 1000) {
        throw std::runtime_error("invalid node presence inspection");
    }
    inspection result{.generation_ = std::string(parts[0].string()), .due_ = {}, .wait_ = std::chrono::milliseconds(parts[2].integer())};
    const auto due = parts[1].array();
    if (due.size() % 2 != 0 || due.size() > 64) {
        throw std::runtime_error("invalid node presence candidates");
    }
    for (std::size_t index = 0; index < due.size(); index += 2) {
        result.due_.push_back({std::string(due[index].string()), std::string(due[index + 1].string())});
    }
    co_return result;
}

// A seed is accepted only for the Redis generation that triggered its DB read.
// Every worker runs this same path; there is no elected seed owner.
inline const std::string seed_script = record_lua + R"lua(
presence_validate(KEYS[1])
if redis.call('GET', KEYS[2]) ~= ARGV[1] then return 0 end
for i = 2, #ARGV, 3 do
  presence_record(KEYS[1], ARGV[i], ARGV[i + 1], tonumber(ARGV[i + 2]) - )lua" +
                                       std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(service::node::online_window).count()) + R"lua()
end
return 1
)lua";

inline const std::string complete_seed_script = std::string(redis_snapshot_cache::invalidation_lua) + R"lua(
if redis.call('GET', KEYS[1]) ~= ARGV[1] then return 0 end
if redis.call('GET', KEYS[2]) == ARGV[1] then return 1 end
local kind = redis.call('TYPE', KEYS[3]).ok
if kind ~= 'none' and kind ~= 'stream' then error('invalid presence notification stream') end
snapshot_validate_generations(KEYS[4], KEYS[5], KEYS[6])
local clock = redis.call('TIME')
local now = tonumber(clock[1]) * 1000 + math.floor(tonumber(clock[2]) / 1000)
redis.call('XADD', KEYS[3], 'MAXLEN', '~', 10000, '*', 'tenant_id', '*', 'missed_until', tostring(now), 'event_id', ARGV[1])
snapshot_invalidate(KEYS[4], KEYS[5], KEYS[6], {'*'}, ARGV[1])
redis.call('SET', KEYS[2], ARGV[1])
return 1
)lua";

inline ruvia::task<bool> complete_seed(ruvia::redis_handle redis, std::string_view generation) {
    const std::array keys{generation_key, seeded_generation_key, notifications::stream_key,
        redis_snapshot_cache::generation_keys[0], redis_snapshot_cache::generation_keys[1], redis_snapshot_cache::generation_keys[2]};
    const std::array arguments{generation};
    const auto result = co_await redis.eval(complete_seed_script, keys, arguments);
    service::utils::require_redis_success(result, "could not finish node presence recovery");
    co_return result.integer() == 1;
}

template <typename context_type>
ruvia::task<bool> seed_from_database(context_type& context, std::string_view generation) {
    ruvia::db_query expressions;
    const auto epoch_ms = expressions.cast(expressions.call("floor", {expressions.binary(expressions.call("date_part", {expressions.value("epoch"), expressions.column("last_heartbeat_at", "node")}),
                                                                         ruvia::db_binary_operator::multiply, expressions.value(1000))}),
        ruvia::db_data_type::big_int);
    const auto deadline = expressions.binary(epoch_ms, ruvia::db_binary_operator::add,
        expressions.value(std::chrono::duration_cast<std::chrono::milliseconds>(service::node::online_window).count()));
    auto repository = context.db().template get_repository<node_presence_entity>();
    auto query = repository.create_query_builder("node");
    query.select({{"id", {}}, {"tenant_id", {}}, {"deadline_unix_ms", deadline}})
        .where(expressions.unary(ruvia::db_unary_operator::is_null, expressions.column("deleted_at", "node")))
        .and_where(service::node::node_online_predicate(expressions, "node"));
    const auto rows = co_await query.template get_many<seed_projection>();
    const std::array keys{deadlines_key, generation_key};
    for (std::size_t start = 0; start < rows.size(); start += 128) {
        std::vector<std::string> values{std::string(generation)};
        const auto end = std::min(rows.size(), start + 128);
        for (std::size_t index = start; index < end; ++index) {
            values.emplace_back(rows[index].template get<"tenant_id">());
            values.emplace_back(rows[index].template get<"id">());
            values.push_back(std::to_string(rows[index].template get<"deadline_unix_ms">()));
        }
        std::vector<std::string_view> arguments;
        for (const auto& value : values) {
            arguments.push_back(value);
        }
        const auto result = co_await context.redis().with_options({.timeout_ = command_timeout,
                                                                      .stop_token_ = context.get_stop_token()})
                                .eval(seed_script, keys, arguments);
        service::utils::require_redis_success(result, "could not seed node presence");
        if (result.integer() != 1) {
            co_return false;
        }
    }
    co_return co_await complete_seed(context.redis().with_options({.timeout_ = command_timeout,
                                         .stop_token_ = context.get_stop_token()}),
        generation);
}

inline const std::string expire_script = std::string(redis_snapshot_cache::invalidation_lua) + R"lua(
local score = redis.call('ZSCORE', KEYS[1], ARGV[1])
if not score or score ~= ARGV[2] then return 0 end
local clock = redis.call('TIME')
local now = tonumber(clock[1]) * 1000 + math.floor(tonumber(clock[2]) / 1000)
if tonumber(score) >= now then return 0 end
local tenant, node = string.match(ARGV[1], '^([^|]+)|([^|]+)$')
if not tenant or not node then error('invalid presence member') end
local kind = redis.call('TYPE', KEYS[2]).ok
if kind ~= 'none' and kind ~= 'stream' then error('invalid presence notification stream') end
snapshot_validate_generations(KEYS[3], KEYS[4], KEYS[5])
local scopes = {}
for i = 1, tonumber(ARGV[4]) do scopes[i] = ARGV[4 + i] end
redis.call('XADD', KEYS[2], 'MAXLEN', '~', 10000, '*', 'tenant_id', tenant, 'changes', 'nodes:' .. node, 'event_id', ARGV[3])
snapshot_invalidate(KEYS[3], KEYS[4], KEYS[5], scopes, ARGV[3])
redis.call('ZREM', KEYS[1], ARGV[1])
return 1
)lua";

inline ruvia::task<bool> expire(ruvia::redis_handle redis, const due_node& candidate) {
    const auto separator = candidate.member_.find('|');
    if (separator == std::string::npos || separator == 0 || separator + 1 == candidate.member_.size()) {
        throw std::runtime_error("invalid node presence identity");
    }
    const auto tenant = std::string_view(candidate.member_).substr(0, separator);
    const auto node = std::string_view(candidate.member_).substr(separator + 1);
    const auto scopes = redis_snapshot_cache::invalidation_scopes(tenant, resource::nodes, node);
    const std::array keys{deadlines_key, notifications::stream_key,
        redis_snapshot_cache::generation_keys[0], redis_snapshot_cache::generation_keys[1], redis_snapshot_cache::generation_keys[2]};
    std::vector<std::string> values{candidate.member_, candidate.deadline_, service::utils::random_token(), std::to_string(scopes.size())};
    values.insert(values.end(), scopes.begin(), scopes.end());
    std::vector<std::string_view> arguments;
    for (const auto& value : values) {
        arguments.push_back(value);
    }
    const auto result = co_await redis.eval(expire_script, keys, arguments);
    service::utils::require_redis_success(result, "could not publish node presence expiry");
    co_return result.integer() == 1;
}

template <typename context_type>
ruvia::task<void> run_worker(context_type& context) {
    // Only connection recovery metadata is local. Node deadlines, expiry
    // claims and notification state remain exclusively in Redis.
    std::string generation;
    while (!context.get_stop_token().stop_requested()) {
        auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(idle_wait);
        try {
            auto pending = co_await inspect(context.redis().with_options({.timeout_ = command_timeout,
                .stop_token_ = context.get_stop_token()}));
            if (pending.generation_ != generation) {
                if (co_await seed_from_database(context, pending.generation_)) {
                    generation = std::move(pending.generation_);
                }
                continue;
            }
            for (const auto& candidate : pending.due_) {
                (void)co_await expire(context.redis().with_options({.timeout_ = command_timeout,
                                          .stop_token_ = context.get_stop_token()}),
                    candidate);
            }
            wait = pending.due_.empty() ? pending.wait_ : std::chrono::milliseconds::zero();
        } catch (const std::exception&) {
            if (context.get_stop_token().stop_requested()) {
                break;
            }
            service::logging::error("Node presence recovery deferred; Redis deadlines remain claimable");
        }
        if (wait > std::chrono::milliseconds::zero() && co_await ruvia::sleep_for(context.worker(), wait,
                                                            context.get_stop_token()) == ruvia::timer_sleep_result::stop_requested) {
            break;
        }
    }
}

}  // namespace service::live_resource::node_presence
