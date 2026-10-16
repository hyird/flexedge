#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/redis/redis.h>

#include <array>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "service/domains/agent/agent_types.h"
#include "service/utils/redis_reply.h"
#include "service/utils/token.h"

namespace service::node_channel {

inline constexpr std::string_view agent_key_prefix{"flexedge:node-channel:v1:agent:"};
inline constexpr std::string_view ready_key{"flexedge:node-channel:v1:ready"};
inline constexpr std::string_view repair_key{"flexedge:node-channel:v1:repair"};
inline constexpr std::string_view identities_key{"flexedge:node-channel:v1:identities"};
inline constexpr std::string_view repair_cooldown_key{"flexedge:node-channel:v1:repair-cooldown"};

inline std::string identity_binding(const service::agent::agent_principal& principal) {
    return principal.tenant_id_ + ":" + principal.node_id_ + ":" + principal.cluster_id_;
}

// This independent index proves only that a repair concerns a current agent.
// It never supplies credentials or authorizes a connection. Keep retries from
// turning damaged Redis bytes into an unbounded source-database scan.
inline ruvia::task<bool> request_repair(ruvia::redis_handle redis, std::string_view agent_id,
    std::string_view binding = {}, std::string_view release_id = {}) {
    static constexpr std::string_view script = R"lua(
local binding = redis.call('HGET', KEYS[3], ARGV[2])
if not binding or (ARGV[3] ~= '' and binding ~= ARGV[3]) then return 0 end
if ARGV[4] ~= '' then
 local release = redis.call('HGET', KEYS[5], 'release_id')
 if release and release ~= ARGV[4] then return 0 end
end
if redis.call('EXISTS', KEYS[1]) == 0 and redis.call('EXISTS', KEYS[2]) == 1 then return 1 end
if not redis.call('SET', KEYS[4], ARGV[1], 'NX', 'EX', 60) then return 0 end
redis.call('SET', KEYS[2], ARGV[1])
redis.call('DEL', KEYS[1])
return 1
)lua";
    const auto key = std::string(agent_key_prefix) + std::string(agent_id);
    const std::array<std::string_view, 5> keys{ready_key, repair_key, identities_key, repair_cooldown_key, key};
    const auto token = service::utils::random_token();
    const std::array<std::string_view, 4> arguments{token, agent_id, binding, release_id};
    const auto result = co_await redis.eval(script, keys, arguments);
    service::utils::require_redis_success(result, "could not request node projection repair");
    co_return result.kind() == ruvia::redis_value::kind_type::integer&& result.integer() == 1;
}

inline ruvia::task<bool> request_repair(ruvia::redis_handle redis,
    const service::agent::agent_principal& principal, std::string_view release_id = {}) {
    const auto binding = identity_binding(principal);
    co_return co_await request_repair(redis, principal.agent_id_, binding, release_id);
}

inline std::string agent_key(std::string_view agent_id) {
    return std::string(agent_key_prefix) + std::string(agent_id);
}

struct identity_record final {
    std::string tenant_id_, node_id_, cluster_id_, agent_id_, secret_hash_, status_;
};

inline ruvia::task<std::optional<identity_record>> identity(ruvia::redis_handle redis,
    std::string_view agent_id) {
    const auto result = co_await redis.command("HMGET", agent_key(agent_id), "tenant_id", "node_id",
        "cluster_id", "agent_id", "secret_hash", "status");
    service::utils::require_redis_success(result, "could not read node channel identity");
    if (result.kind() != ruvia::redis_value::kind_type::array || result.array().size() != 6) {
        throw std::runtime_error("invalid node channel identity reply");
    }
    const auto fields = result.array();
    if (fields[0].null()) {
        (void)co_await request_repair(redis, agent_id);
        co_return std::nullopt;
    }
    for (const auto& field : fields) {
        if (field.kind() != ruvia::redis_value::kind_type::string || field.string().empty()) {
            (void)co_await request_repair(redis, agent_id);
            co_return std::nullopt;
        }
    }
    identity_record value{std::string(fields[0].string()), std::string(fields[1].string()),
        std::string(fields[2].string()), std::string(fields[3].string()),
        std::string(fields[4].string()), std::string(fields[5].string())};
    if (value.agent_id_ != agent_id || (value.status_ != "pending" && value.status_ != "registered")) {
        (void)co_await request_repair(redis, agent_id);
        co_return std::nullopt;
    }
    co_return value;
}

// Read the identity and payload in one Redis operation. A connection's prior
// authentication never authorizes bytes belonging to a rotated agent or release.
inline constexpr std::string_view read_script = R"lua(
local fields = redis.call('HMGET', KEYS[1], 'tenant_id', 'node_id', 'cluster_id', 'agent_id', 'status', 'release_id')
if fields[1] ~= ARGV[1] or fields[2] ~= ARGV[2] or fields[3] ~= ARGV[3] or fields[4] ~= ARGV[4] or fields[5] ~= 'registered' then return nil end
if ARGV[5] ~= '' and fields[6] ~= ARGV[5] then return nil end
local result = {}
for i = 6, #ARGV do
 local value = redis.call('HGET', KEYS[1], ARGV[i])
 if not value then return nil end
 result[#result + 1] = value
end
return result
)lua";

inline ruvia::task<std::optional<std::string>> desired(ruvia::redis_handle redis,
    const service::agent::agent_principal& principal) {
    const auto key = agent_key(principal.agent_id_);
    const std::array<std::string_view, 1> keys{key};
    const std::array<std::string_view, 6> arguments{principal.tenant_id_, principal.node_id_,
        principal.cluster_id_, principal.agent_id_, "", "desired"};
    const auto result = co_await redis.eval(read_script, keys, arguments);
    service::utils::require_redis_success(result, "could not read desired node state");
    if (result.null()) {
        co_return std::nullopt;
    }
    if (result.kind() != ruvia::redis_value::kind_type::array || result.array().size() != 1 ||
        result.array()[0].kind() != ruvia::redis_value::kind_type::string) {
        throw std::runtime_error("invalid desired node state reply");
    }
    co_return std::string(result.array()[0].string());
}

inline void validate_object_request(const std::vector<std::string>& requested) {
    std::unordered_set<std::string_view> unique;
    for (const auto& digest : requested) {
        if (digest.size() != 64 || digest.find_first_not_of("0123456789abcdef") != std::string::npos ||
            !unique.insert(digest).second) {
            throw std::invalid_argument("invalid node object request");
        }
    }
}

class projection_unavailable final : public std::runtime_error {
public:
    projection_unavailable()
        : std::runtime_error("node release projection is unavailable") {}
};

inline ruvia::task<std::vector<std::string>> objects(ruvia::redis_handle redis,
    const service::agent::agent_principal& principal, std::string_view release_id,
    const std::vector<std::string>& requested) {
    if (release_id.empty()) {
        throw std::invalid_argument("node release identity is required");
    }
    validate_object_request(requested);
    std::vector<std::string> fields;
    fields.reserve(requested.size());
    for (const auto& digest : requested) {
        fields.push_back("object:" + digest);
    }
    const auto key = agent_key(principal.agent_id_);
    const std::array<std::string_view, 1> keys{key};
    std::vector<std::string_view> arguments{principal.tenant_id_, principal.node_id_,
        principal.cluster_id_, principal.agent_id_, release_id};
    for (const auto& field : fields) {
        arguments.push_back(field);
    }
    const auto result = co_await redis.eval(read_script, keys, arguments);
    service::utils::require_redis_success(result, "could not read node release objects");
    if (result.null()) {
        throw projection_unavailable();
    }
    if (result.kind() != ruvia::redis_value::kind_type::array || result.array().size() != requested.size()) {
        throw std::runtime_error("invalid node release objects reply");
    }
    std::vector<std::string> values;
    values.reserve(requested.size());
    for (const auto& value : result.array()) {
        if (value.kind() != ruvia::redis_value::kind_type::string) {
            throw std::runtime_error("invalid node release object");
        }
        values.emplace_back(value.string());
    }
    co_return values;
}

}  // namespace service::node_channel
