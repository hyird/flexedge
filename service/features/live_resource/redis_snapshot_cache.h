#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/redis/redis.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <openssl/evp.h>

#include "service/features/live_resource/query_key.h"
#include "service/features/live_resource/snapshot_reads.h"
#include "service/utils/redis_reply.h"
#include "service/utils/token.h"

namespace service::live_resource::redis_snapshot_cache {

// Worker-owned subscriptions coalesce only concurrent reads and discard
// interrupted flights. Redis alone retains reusable successful serialized
// tenant projections; session checks and authorization always run separately.
inline constexpr std::size_t max_entries{1024};
inline constexpr std::size_t max_bytes{16 * 1024 * 1024};
inline constexpr std::size_t max_snapshot_bytes{256 * 1024};
inline constexpr std::size_t max_scopes{4096};
inline constexpr auto max_age = std::chrono::seconds(15);
inline constexpr auto operation_timeout = std::chrono::milliseconds(200);
inline constexpr std::array<std::string_view, 6> keys{
    "flexedge:{snapshot-cache:v1}:values", "flexedge:{snapshot-cache:v1}:expiry",
    "flexedge:{snapshot-cache:v1}:bytes", "flexedge:{snapshot-cache:v1}:epochs",
    "flexedge:{snapshot-cache:v1}:epoch-index", "flexedge:{snapshot-cache:v1}:sequence"};
inline constexpr std::array<std::string_view, 3> generation_keys{keys[3], keys[4], keys[5]};

[[nodiscard]] inline bool cacheable(const snapshot_key& key) {
    return !key.tenant_.empty() &&
           ((key.resource_ == resource::overview && key.id_.empty() && key.query_ == query_key("overview")) ||
               (key.resource_ == resource::overview_traffic && key.id_.empty() && key.query_ == query_key("traffic")) ||
               (key.resource_ == resource::access_history && !key.id_.empty() && key.query_ == query_key("dashboard")));
}

[[nodiscard]] inline std::string digest(std::string_view bytes) {
    std::array<unsigned char, 32> result{};
    unsigned int size{};
    if (EVP_Digest(bytes.data(), bytes.size(), result.data(), &size, EVP_sha256(), nullptr) != 1 || size != result.size()) {
        throw std::runtime_error("snapshot cache key digest failed");
    }
    constexpr std::string_view hex{"0123456789abcdef"};
    std::string value;
    value.reserve(result.size() * 2);
    for (const auto byte : result) {
        value += hex[byte >> 4U];
        value += hex[byte & 15U];
    }
    return value;
}

[[nodiscard]] inline std::string tenant_scope(std::string_view tenant) {
    return digest(query_key("tenant", tenant));
}

[[nodiscard]] inline std::string resource_scope(std::string_view tenant, resource resource_value, std::string_view id = {}) {
    return digest(query_key("resource", tenant, static_cast<int>(resource_value), id));
}

[[nodiscard]] inline std::string entry_key(const snapshot_key& key) {
    return digest(query_key("projection", key.tenant_, static_cast<int>(key.resource_), key.id_, key.query_));
}

[[nodiscard]] inline std::vector<std::string> scopes(const snapshot_key& key) {
    std::vector<std::string> result{"*", tenant_scope(key.tenant_), resource_scope(key.tenant_, key.resource_)};
    if (!key.id_.empty()) {
        result.push_back(resource_scope(key.tenant_, key.resource_, key.id_));
    }
    return result;
}

// Mirrors the dependency rules of live_resource::hub for the three cached
// projections. Only actual changed resources invalidate; node/origin runtime
// patches do not refresh these database aggregates.
[[nodiscard]] inline std::vector<std::string> invalidation_scopes(std::string_view tenant,
    resource resource_value, std::string_view id = {}) {
    std::vector<std::string> result;
    const auto add = [&](resource value, std::string_view resource_id = {}) {
        result.push_back(resource_scope(tenant, value, resource_id));
    };
    if (resource_value != resource::tasks && resource_value != resource::access_history &&
        resource_value != resource::overview_traffic) {
        add(resource::overview);
    }
    if (resource_value == resource::overview_traffic || resource_value == resource::websites) {
        add(resource::overview_traffic);
    }
    if (resource_value == resource::websites || resource_value == resource::access_history) {
        add(resource::access_history, id);
    }
    return result;
}

// Embed this prelude in atomic stream publication scripts. Before mutating
// their stream, call snapshot_validate_generations(KEYS[a], KEYS[b], KEYS[c]);
// afterwards call snapshot_invalidate(KEYS[a], KEYS[b], KEYS[c], scopes, event_id).
// A missing/evicted scope gets a fresh sequence, so even metadata eviction
// cannot make an older in-flight query valid again. The global scope is
// permanent; all other metadata is bounded and contains hashes, never IDs.
inline constexpr std::string_view invalidation_lua = R"lua(
local function snapshot_validate_generations(epochs, epoch_index, sequence)
  local expected = {{epochs, 'hash'}, {epoch_index, 'zset'}, {sequence, 'string'}}
  for _, pair in ipairs(expected) do
    local kind = redis.call('TYPE', pair[1]).ok
    if kind ~= 'none' and kind ~= pair[2] then error('invalid snapshot generation key type') end
  end
  local value = redis.call('GET', sequence)
  if value and (not string.match(value, '^%d+$') or #value > 18) then
    error('invalid snapshot generation sequence')
  end
end
local function snapshot_next_sequence(sequence, nonce)
  redis.call('INCR', sequence)
  return nonce .. ':' .. redis.call('GET', sequence)
end
local function snapshot_touch_scope(epochs, epoch_index, sequence, scope, now, nonce)
  local value = redis.call('HGET', epochs, scope)
  if not value then
    value = snapshot_next_sequence(sequence, nonce)
    redis.call('HSET', epochs, scope, value)
  end
  if scope ~= '*' then redis.call('ZADD', epoch_index, now, scope) end
  return value
end
local function snapshot_prune_scopes(epochs, epoch_index)
  local excess = redis.call('ZCARD', epoch_index) - 4095
  if excess > 0 then
    local oldest = redis.call('ZRANGE', epoch_index, 0, excess - 1)
    for _, scope in ipairs(oldest) do
      redis.call('HDEL', epochs, scope)
      redis.call('ZREM', epoch_index, scope)
    end
  end
end
local function snapshot_invalidate(epochs, epoch_index, sequence, scopes, nonce)
  local clock = redis.call('TIME')
  local now = tonumber(clock[1]) * 1000000 + tonumber(clock[2])
  for _, scope in ipairs(scopes) do
    redis.call('HSET', epochs, scope, snapshot_next_sequence(sequence, nonce))
    if scope ~= '*' then redis.call('ZADD', epoch_index, now, scope) end
  end
  snapshot_prune_scopes(epochs, epoch_index)
end
)lua";

namespace detail {

inline const std::string cache_lua = std::string(invalidation_lua) + R"lua(
snapshot_validate_generations(KEYS[4], KEYS[5], KEYS[6])
for _, pair in ipairs({{KEYS[1], 'hash'}, {KEYS[2], 'zset'}, {KEYS[3], 'string'}}) do
  local kind = redis.call('TYPE', pair[1]).ok
  if kind ~= 'none' and kind ~= pair[2] then error('invalid snapshot cache key type') end
end
local clock = redis.call('TIME')
local now = tonumber(clock[1]) * 1000 + math.floor(tonumber(clock[2]) / 1000)
local used = tonumber(redis.call('GET', KEYS[3]) or '0')
if not used or used < 0 then error('invalid snapshot cache byte counter') end
local function remove_entry(id)
  local value = redis.call('HGET', KEYS[1], id)
  if value then used = math.max(0, used - #value) end
  redis.call('HDEL', KEYS[1], id)
  redis.call('ZREM', KEYS[2], id)
end
local expired = redis.call('ZRANGEBYSCORE', KEYS[2], '-inf', now)
for _, id in ipairs(expired) do remove_entry(id) end
redis.call('SET', KEYS[3], tostring(used))
local scope_count = tonumber(ARGV[2])
local generation = {}
for index = 1, scope_count do
  local scope = ARGV[2 + index]
  if ARGV[1] == 'read' then
    generation[index] = snapshot_touch_scope(KEYS[4], KEYS[5], KEYS[6], scope,
      tonumber(clock[1]) * 1000000 + tonumber(clock[2]), ARGV[#ARGV])
  else
    local value = redis.call('HGET', KEYS[4], scope)
    if not value then return 0 end
    generation[index] = value
  end
end
snapshot_prune_scopes(KEYS[4], KEYS[5])
generation = table.concat(generation, '/')
local offset = 3 + scope_count
local id = ARGV[offset]
if ARGV[1] == 'read' then
  local value = redis.call('HGET', KEYS[1], id)
  if value then
    local separator = string.find(value, '\n', 1, true)
    if separator and string.sub(value, 1, separator - 1) == generation then
      local expiry = tonumber(redis.call('ZSCORE', KEYS[2], id) or '0')
      if expiry > now then return {generation, string.sub(value, separator + 1), expiry - now} end
    end
    remove_entry(id)
    redis.call('SET', KEYS[3], tostring(used))
  end
  return {generation, false, 0}
end
if generation ~= ARGV[offset + 1] then return 0 end
local bytes = ARGV[offset + 2]
if #bytes > 262144 then return 1 end
local value = generation .. '\n' .. bytes
remove_entry(id)
while redis.call('HLEN', KEYS[1]) >= 1024 or used + #value > 16777216 do
  local oldest = redis.call('ZRANGE', KEYS[2], 0, 0)
  if #oldest == 0 then return redis.error_reply('inconsistent snapshot cache index') end
  remove_entry(oldest[1])
end
redis.call('HSET', KEYS[1], id, value)
redis.call('ZADD', KEYS[2], now + 15000, id)
used = used + #value
redis.call('SET', KEYS[3], tostring(used))
return 1
)lua";

inline std::vector<std::string> arguments(std::string operation, const snapshot_key& key) {
    auto result = scopes(key);
    result.insert(result.begin(), std::to_string(result.size()));
    result.insert(result.begin(), std::move(operation));
    result.push_back(entry_key(key));
    return result;
}

inline std::vector<std::string_view> views(const std::vector<std::string>& values) {
    std::vector<std::string_view> result;
    result.reserve(values.size());
    for (const auto& value : values) {
        result.push_back(value);
    }
    return result;
}

}  // namespace detail

struct lookup final {
    std::string generation_;
    std::optional<snapshot> snapshot_;
    snapshot_reads::clock::duration max_age_{};
};

// Failures deliberately propagate to the caller, which falls back to its
// database loader. No Redis error is interpreted as a successful cache hit.
inline ruvia::task<lookup> read(ruvia::redis_handle redis, const snapshot_key& key) {
    const auto started = snapshot_reads::clock::now();
    auto arguments = detail::arguments("read", key);
    arguments.push_back(service::utils::random_token());
    const auto values = detail::views(arguments);
    const auto result = co_await redis.eval(detail::cache_lua, keys, values);
    service::utils::require_redis_success(result, "read snapshot cache");
    if (result.kind() != ruvia::redis_value::kind_type::array || result.array().size() != 3) {
        throw std::runtime_error("invalid snapshot cache read reply");
    }
    const auto parts = result.array();
    if (parts[0].kind() != ruvia::redis_value::kind_type::string ||
        parts[2].kind() != ruvia::redis_value::kind_type::integer) {
        throw std::runtime_error("invalid snapshot cache generation reply");
    }
    lookup output{.generation_ = std::string(parts[0].string()), .snapshot_ = {}, .max_age_ = {}};
    if (!parts[1].null()) {
        if (parts[1].kind() != ruvia::redis_value::kind_type::string || parts[1].string().size() > max_snapshot_bytes) {
            throw std::runtime_error("invalid snapshot cache bytes reply");
        }
        const auto remaining = std::chrono::milliseconds(parts[2].integer()) - (snapshot_reads::clock::now() - started);
        if (remaining > snapshot_reads::clock::duration::zero()) {
            output.snapshot_ = snapshot{std::string(parts[1].string()), false};
            output.max_age_ = std::min<snapshot_reads::clock::duration>(remaining, max_age);
        }
    }
    co_return output;
}

// False means a commit changed the generation during the database flight.
// The caller discards that result and reads the new generation. Oversized
// successful snapshots are delivered normally but never retained in Redis.
inline ruvia::task<bool> store(ruvia::redis_handle redis, const snapshot_key& key,
    std::string_view generation, const snapshot& value) {
    if (value.failure_) {
        co_return true;
    }
    auto arguments = detail::arguments("write", key);
    arguments.emplace_back(generation);
    arguments.emplace_back(value.data_);
    const auto values = detail::views(arguments);
    const auto result = co_await redis.eval(detail::cache_lua, keys, values);
    service::utils::require_redis_success(result, "store snapshot cache");
    if (result.kind() != ruvia::redis_value::kind_type::integer) {
        throw std::runtime_error("invalid snapshot cache store reply");
    }
    co_return result.integer() == 1;
}

inline ruvia::task<void> invalidate(ruvia::redis_handle redis, const std::vector<std::string>& scopes_value) {
    if (scopes_value.empty()) {
        co_return;
    }
    std::vector<std::string> owned_arguments{service::utils::random_token()};
    owned_arguments.insert(owned_arguments.end(), scopes_value.begin(), scopes_value.end());
    const auto arguments = detail::views(owned_arguments);
    const auto script = std::string(invalidation_lua) + R"lua(
snapshot_validate_generations(KEYS[1], KEYS[2], KEYS[3])
local scopes = {}
for index = 2, #ARGV do scopes[#scopes + 1] = ARGV[index] end
snapshot_invalidate(KEYS[1], KEYS[2], KEYS[3], scopes, ARGV[1])
return 1
)lua";
    const auto result = co_await redis.eval(script, generation_keys, arguments);
    service::utils::require_redis_success(result, "invalidate snapshot cache");
    if (result.kind() != ruvia::redis_value::kind_type::integer || result.integer() != 1) {
        throw std::runtime_error("invalid snapshot cache invalidation reply");
    }
}

}  // namespace service::live_resource::redis_snapshot_cache
