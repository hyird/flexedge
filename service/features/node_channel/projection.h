#pragma once

#include <ruvia/core/timer.h>
#include <ruvia/web/db/db.h>

#include <array>
#include <chrono>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "node/proto/artifact.h"
#include "node/proto/edge_control.pb.h"
#include "service/features/live_resource/notification_outbox.h"
#include "service/features/live_resource/notifications.h"
#include "service/features/logging/logger.h"
#include "service/features/node_channel/projection_entity.h"
#include "service/features/node_channel/release_manifest.h"
#include "service/features/node_channel/store.h"
#include "service/features/node_dispatch/protocol.h"
#include "service/utils/secret.h"
#include "service/utils/sensitive_string.h"

namespace service::node_channel {

inline constexpr std::string_view tenants_key{"flexedge:node-channel:v1:tenants"};
inline constexpr auto projection_command_timeout = std::chrono::seconds(5);

inline std::string node_index_key(std::string_view tenant_id, std::string_view node_id) {
    return "flexedge:node-channel:v1:node:" + std::string(tenant_id) + ":" + std::string(node_id);
}
inline std::string tenant_nodes_key(std::string_view tenant_id) {
    return "flexedge:node-channel:v1:tenant:" + std::string(tenant_id) + ":nodes";
}

namespace projection_detail {

inline constexpr std::string_view write_script = R"lua(
local function require_type(key, expected)
 local kind = redis.call('TYPE', key).ok
 if kind ~= 'none' and kind ~= expected then error('invalid node projection key type') end
end
require_type(KEYS[1], 'hash')
require_type(KEYS[2], 'set')
require_type(KEYS[3], 'set')
require_type(KEYS[4], 'hash')
local previous = redis.call('HMGET', KEYS[1], 'agent_id', 'token')
if ARGV[4] == '-' then
 if previous[2] then return 0 end
elseif ARGV[4] ~= '' and previous[2] ~= ARGV[4] then return 0 end
local old_key = previous[1] and ARGV[5] .. previous[1] or ''
local new_key = ARGV[3] ~= '' and ARGV[5] .. ARGV[3] or ''
if old_key ~= '' then require_type(old_key, 'hash') end
if new_key ~= '' then
 require_type(new_key, 'hash')
 local owner = redis.call('HMGET', new_key, 'tenant_id', 'node_id')
 if owner[1] and (owner[1] ~= ARGV[1] or owner[2] ~= ARGV[2]) then error('node agent identity collision') end
 local binding = redis.call('HGET', KEYS[4], ARGV[3])
 local prefix = ARGV[1] .. ':' .. ARGV[2] .. ':'
 if binding and string.sub(binding, 1, #prefix) ~= prefix then error('node agent index collision') end
end
if old_key ~= '' and old_key ~= new_key then
 local owner = redis.call('HMGET', old_key, 'tenant_id', 'node_id')
 if owner[1] == ARGV[1] and owner[2] == ARGV[2] then redis.call('DEL', old_key) end
 local binding = redis.call('HGET', KEYS[4], previous[1])
 local prefix = ARGV[1] .. ':' .. ARGV[2] .. ':'
 if binding and string.sub(binding, 1, #prefix) == prefix then redis.call('HDEL', KEYS[4], previous[1]) end
end
if new_key == '' then
 redis.call('DEL', KEYS[1])
 redis.call('SREM', KEYS[2], ARGV[2])
 if redis.call('SCARD', KEYS[2]) == 0 then
  redis.call('DEL', KEYS[2])
  redis.call('SREM', KEYS[3], ARGV[1])
 end
else
 redis.call('DEL', new_key)
 redis.call('HSET', new_key, 'projection_token', ARGV[6])
 for i = 8, #ARGV, 2 do redis.call('HSET', new_key, ARGV[i], ARGV[i + 1]) end
 redis.call('HSET', KEYS[4], ARGV[3], ARGV[7])
 redis.call('HSET', KEYS[1], 'agent_id', ARGV[3], 'token', ARGV[6])
 redis.call('SADD', KEYS[2], ARGV[2])
 redis.call('SADD', KEYS[3], ARGV[1])
end
return 1
)lua";

inline ruvia::task<void> write(ruvia::redis_handle redis, std::string_view tenant_id,
    std::string_view node_id, std::string_view agent_id,
    const std::vector<std::pair<std::string, std::string>>& fields,
    std::string_view expected_token = {}) {
    const std::array<std::string, 3> key_values{node_index_key(tenant_id, node_id),
        tenant_nodes_key(tenant_id), std::string(tenants_key)};
    const std::array<std::string_view, 4> keys{key_values[0], key_values[1], key_values[2], identities_key};
    const auto token = service::utils::random_token();
    std::string binding;
    if (!agent_id.empty()) {
        for (const auto& [field, value] : fields) {
            if (field == "cluster_id") {
                binding = std::string(tenant_id) + ":" + std::string(node_id) + ":" + value;
                break;
            }
        }
        if (binding.empty()) {
            throw std::logic_error("node projection identity requires a cluster");
        }
    }
    std::vector<std::string_view> arguments{tenant_id, node_id, agent_id, expected_token, agent_key_prefix, token, binding};
    for (const auto& [field, value] : fields) {
        arguments.push_back(field);
        arguments.push_back(value);
    }
    const auto result = co_await redis.eval(write_script, keys, arguments);
    service::utils::require_redis_success(result, "could not replace node channel projection");
}

inline std::vector<std::string> members(const ruvia::redis_value& result) {
    service::utils::require_redis_success(result, "could not read node projection index");
    if (result.kind() != ruvia::redis_value::kind_type::array) {
        throw std::runtime_error("invalid node projection index reply");
    }
    std::vector<std::string> values;
    for (const auto& value : result.array()) {
        if (value.kind() != ruvia::redis_value::kind_type::string) {
            throw std::runtime_error("invalid node projection index member");
        }
        values.emplace_back(value.string());
    }
    return values;
}

inline ruvia::db_expression equal(ruvia::db_query& query, std::string_view column,
    std::string_view value, std::string_view alias = {}) {
    return query.binary(query.column(column, alias), ruvia::db_binary_operator::equal, query.value(value));
}

template <typename context_type>
ruvia::task<void> prune_orphan_agents(context_type& context) {
    auto redis = context.redis().with_options({.timeout_ = projection_command_timeout,
        .stop_token_ = context.get_stop_token()});
    std::string cursor{"0"};
    // The repair index can outlive its payload HASH after partial Redis loss.
    // Reconcile it independently so revoked ids cannot keep requesting scans.
    do {
        const auto scan = co_await redis.command("HSCAN", identities_key, cursor, "COUNT", "128");
        service::utils::require_redis_success(scan, "could not scan node repair identities");
        if (scan.kind() != ruvia::redis_value::kind_type::array || scan.array().size() != 2) {
            throw std::runtime_error("invalid node repair identity scan reply");
        }
        cursor = std::string(scan.array()[0].string());
        const auto entries = members(scan.array()[1]);
        if (entries.size() % 2 != 0) {
            throw std::runtime_error("invalid node repair identity entries");
        }
        for (std::size_t index = 0; index < entries.size(); index += 2) {
            const auto& agent_id = entries[index];
            const auto& binding = entries[index + 1];
            const auto first = binding.find(':');
            const auto second = first == std::string::npos ? first : binding.find(':', first + 1);
            if (first == std::string::npos || second == std::string::npos) {
                throw std::runtime_error("invalid node repair identity binding");
            }
            const auto tenant_id = binding.substr(0, first);
            const auto node_id = binding.substr(first + 1, second - first - 1);
            const auto cluster_id = binding.substr(second + 1);
            auto transaction = co_await context.db().begin_transaction();
            ruvia::db_query expressions;
            auto query = transaction.template get_repository<projection_node_entity>().create_query_builder();
            query.where(equal(expressions, "tenant_id", tenant_id)).and_where(equal(expressions, "id", node_id)).set_lock({.mode_ = ruvia::db_row_lock::update});
            const auto node = co_await query.get_one();
            if (!node || !node->template is_null<"deleted_at">() || node->template is_null<"agent_id">() ||
                node->template is_null<"node_secret_hash">() ||
                std::string_view(node->template get<"agent_id">()) != agent_id ||
                std::string_view(node->template get<"cluster_id">()) != cluster_id) {
                static constexpr std::string_view remove_script = R"lua(
if redis.call('HGET', KEYS[2], ARGV[1]) ~= ARGV[2] then return 0 end
local owner = redis.call('HMGET', KEYS[1], 'tenant_id', 'node_id')
if not owner[1] or (owner[1] == ARGV[3] and owner[2] == ARGV[4]) then redis.call('DEL', KEYS[1]) end
redis.call('HDEL', KEYS[2], ARGV[1])
return 1
)lua";
                const auto key = agent_key(agent_id);
                const std::array<std::string_view, 2> keys{key, identities_key};
                const std::array<std::string_view, 4> arguments{agent_id, binding, tenant_id, node_id};
                service::utils::require_redis_success(co_await redis.eval(remove_script, keys, arguments), "could not prune node repair identity");
            }
            co_await transaction.commit();
        }
    } while (cursor != "0");
    do {
        const auto scan = co_await redis.command("SCAN", cursor, "MATCH", std::string(agent_key_prefix) + "*", "COUNT", "128");
        service::utils::require_redis_success(scan, "could not scan node projection identities");
        if (scan.kind() != ruvia::redis_value::kind_type::array || scan.array().size() != 2) {
            throw std::runtime_error("invalid node projection scan reply");
        }
        cursor = std::string(scan.array()[0].string());
        for (const auto& key : members(scan.array()[1])) {
            const auto identity = co_await redis.command("HMGET", key, "tenant_id", "node_id", "agent_id", "projection_token");
            service::utils::require_redis_success(identity, "could not inspect projected node identity");
            const auto fields = identity.array();
            if (fields.size() != 4 || fields[0].null() || fields[1].null() || fields[2].null() || fields[3].null()) {
                continue;
            }
            const auto tenant_id = std::string(fields[0].string());
            const auto node_id = std::string(fields[1].string());
            auto transaction = co_await context.db().begin_transaction();
            ruvia::db_query expressions;
            auto query = transaction.template get_repository<projection_node_entity>().create_query_builder();
            query.where(equal(expressions, "tenant_id", tenant_id)).and_where(equal(expressions, "id", node_id)).set_lock({.mode_ = ruvia::db_row_lock::update});
            const auto node = co_await query.get_one();
            if (!node || !node->template is_null<"deleted_at">() || node->template is_null<"agent_id">() ||
                std::string_view(node->template get<"agent_id">()) != fields[2].string()) {
                static constexpr std::string_view remove_script = R"lua(
if redis.call('HGET', KEYS[1], 'projection_token') ~= ARGV[1] then return 0 end
redis.call('DEL', KEYS[1])
return 1
)lua";
                const std::array<std::string_view, 1> keys{key};
                const std::array<std::string_view, 1> arguments{fields[3].string()};
                const auto result = co_await redis.eval(remove_script, keys, arguments);
                service::utils::require_redis_success(result, "could not prune obsolete node identity");
            }
            co_await transaction.commit();
        }
    } while (cursor != "0");
}

inline ruvia::task<void> refresh_node(ruvia::db_transaction& transaction,
    ruvia::redis_handle redis, std::string_view tenant_id, std::string_view node_id) {
    // A missing SQL row cannot be locked. Capture its Redis version first so
    // cleanup cannot remove a concurrently created projection with the same id.
    const auto previous = co_await redis.command("HGET", node_index_key(tenant_id, node_id), "token");
    service::utils::require_redis_success(previous, "could not read node projection version");
    const auto previous_token = previous.null() ? std::string{} : std::string(previous.string());
    ruvia::db_query expressions;
    auto node_query = transaction.template get_repository<projection_node_entity>().create_query_builder();
    node_query.where(equal(expressions, "tenant_id", tenant_id)).and_where(equal(expressions, "id", node_id)).set_lock({.mode_ = ruvia::db_row_lock::update});
    const auto node = co_await node_query.get_one();
    if (!node) {
        co_await write(redis, tenant_id, node_id, {}, {}, previous_token.empty() ? std::string_view{"-"} : std::string_view{previous_token});
        co_return;
    }
    if (!node->template is_null<"deleted_at">() || node->template is_null<"agent_id">() ||
        node->template is_null<"node_secret_hash">()) {
        co_await write(redis, tenant_id, node_id, {}, {});
        co_return;
    }
    const auto agent_id = std::string(node->template get<"agent_id">());
    const auto cluster_id = std::string(node->template get<"cluster_id">());
    const auto status = std::string(node->template get<"registration_status">());
    std::vector<std::pair<std::string, std::string>> fields{
        {"tenant_id", std::string(tenant_id)}, {"node_id", std::string(node_id)},
        {"cluster_id", cluster_id}, {"agent_id", agent_id},
        {"secret_hash", std::string(node->template get<"node_secret_hash">())}, {"status", status}};
    std::optional<std::string> digest_to_record;
    if (status == "registered") {
        if (node->template is_null<"desired_release_id">()) {
            throw std::runtime_error("registered node has no desired release");
        }
        const auto release_id = std::string(node->template get<"desired_release_id">());
        auto release_query = transaction.template get_repository<projection_release_entity>().create_query_builder();
        release_query.where(equal(expressions, "tenant_id", tenant_id)).and_where(equal(expressions, "id", release_id)).and_where(equal(expressions, "cluster_id", cluster_id));
        const auto release = co_await release_query.get_one();
        if (!release || release->template is_null<"manifest_digest">() || release->template is_null<"manifest_envelope">()) {
            throw std::runtime_error("node desired release is unavailable");
        }
        const auto manifest_digest = std::string(release->template get<"manifest_digest">());
        service::utils::sensitive_string manifest_plaintext(service::utils::open_secret(release->template get<"manifest_envelope">()));
        const auto manifest = parse_release_manifest(manifest_plaintext.view(), manifest_digest, release_id, cluster_id);
        const auto release_content = manifest ? flexedge::node::open_artifact<flexedge::node::v2::ClusterReleaseContent>(
                                                    manifest->digest_sha256(), manifest->content())
                                              : std::nullopt;
        if (!release_content) {
            throw std::runtime_error("node release manifest is invalid");
        }
        flexedge::node::v2::DesiredState state;
        *state.mutable_node_spec() = service::node_dispatch::build_node_spec(node_id,
            node->template get<"node_spec_revision">(), node->template get<"name">(),
            node->template get<"status">() == "enabled", node->template get<"config">());
        *state.mutable_release() = *manifest;
        const auto& spec_digest = state.node_spec().digest_sha256();
        if (node->template is_null<"node_spec_digest">() || std::string_view(node->template get<"node_spec_digest">()) != spec_digest) {
            digest_to_record = spec_digest;
        }
        std::unordered_map<std::string, flexedge::node::v2::ObjectKind> expected;
        for (const auto& object : release_content->objects()) {
            if (!expected.emplace(object.digest_sha256(), object.kind()).second) {
                throw std::runtime_error("duplicate release object");
            }
        }
        auto objects_query = transaction.template get_repository<projection_object_entity>().create_query_builder("object");
        const auto same_tenant = expressions.binary(expressions.column("tenant_id", "object"), ruvia::db_binary_operator::equal,
            expressions.column("tenant_id", "mapping"));
        const auto same_digest = expressions.binary(expressions.column("digest_sha256", "object"), ruvia::db_binary_operator::equal,
            expressions.column("object_digest", "mapping"));
        objects_query.template join<projection_mapping_entity>(ruvia::db_join_type::inner, "mapping",
                         expressions.binary(same_tenant, ruvia::db_binary_operator::and_value, same_digest))
            .where(equal(expressions, "tenant_id", tenant_id, "mapping"))
            .and_where(equal(expressions, "release_id", release_id, "mapping"));
        const auto objects = co_await objects_query.get_many();
        for (const auto& object : objects) {
            const auto digest = std::string(object.template get<"digest_sha256">());
            const auto found = expected.find(digest);
            if (found == expected.end()) {
                throw std::runtime_error("release object is absent from manifest");
            }
            service::utils::sensitive_string plaintext(service::utils::open_secret(object.template get<"payload_envelope">()));
            const auto content = flexedge::node::open_artifact<flexedge::node::v2::DeliveryObjectContent>(digest, plaintext.view());
            if (!content ||
                (found->second == flexedge::node::v2::OBJECT_KIND_WEBSITE && (!content->has_website() || object.template get<"kind">() != "website")) ||
                (found->second == flexedge::node::v2::OBJECT_KIND_CERTIFICATE && (!content->has_certificate() || object.template get<"kind">() != "certificate")) ||
                (found->second != flexedge::node::v2::OBJECT_KIND_WEBSITE && found->second != flexedge::node::v2::OBJECT_KIND_CERTIFICATE)) {
                throw std::runtime_error("node release object is invalid");
            }
            fields.emplace_back("object:" + digest, object.template get<"payload_envelope">());
            expected.erase(found);
        }
        if (!expected.empty()) {
            throw std::runtime_error("node release object is missing");
        }
        service::utils::sensitive_string state_plaintext(flexedge::node::serialize_artifact(state));
        fields.emplace_back("desired", service::utils::seal_secret(state_plaintext.view()));
        fields.emplace_back("release_id", release_id);
        fields.emplace_back("revision", std::to_string(node->template get<"node_spec_revision">()));
        fields.emplace_back("manifest_digest", manifest_digest);
    } else if (status != "pending") {
        throw std::runtime_error("invalid node registration status");
    }
    // The node row remains locked across the Redis write. A newer source
    // transaction cannot commit and then be overwritten by this older reader.
    if (digest_to_record) {
        ruvia::db_query record;
        record.update(projection_node_entity::table_name()).set("node_spec_digest", record.value(std::string_view(*digest_to_record))).where(equal(record, "tenant_id", tenant_id)).and_where(equal(record, "id", node_id));
        (void)co_await transaction.execute(record);
    }
    co_await write(redis, tenant_id, node_id, agent_id, fields);
}

template <typename context_type>
ruvia::task<void> refresh_node(context_type& context, std::string_view tenant_id, std::string_view node_id) {
    auto redis = context.redis().with_options({.timeout_ = projection_command_timeout,
        .stop_token_ = context.get_stop_token()});
    auto transaction = co_await context.db().begin_transaction();
    co_await refresh_node(transaction, redis, tenant_id, node_id);
    co_await transaction.commit();
}

inline ruvia::task<void> clean_tenant_index(ruvia::redis_handle redis, std::string_view tenant_id) {
    static constexpr std::string_view script = R"lua(
if redis.call('SCARD', KEYS[1]) == 0 then redis.call('SREM', KEYS[2], ARGV[1]) end
return 1
)lua";
    const auto index = tenant_nodes_key(tenant_id);
    const std::array<std::string_view, 2> keys{index, tenants_key};
    const std::array<std::string_view, 1> arguments{tenant_id};
    service::utils::require_redis_success(co_await redis.eval(script, keys, arguments), "could not clean node projection tenant index");
}

}  // namespace projection_detail

// Delivery already owns this Worker's sole SQL connection. Reuse its claim
// transaction, including its row locks, instead of waiting for a second lease.
// The source business transaction must have committed before entering here.
inline ruvia::task<void> refresh(ruvia::db_transaction& transaction,
    ruvia::redis_handle redis, std::string_view tenant) {
    ruvia::db_query expressions;
    auto query = transaction.get_repository<projection_node_entity>().create_query_builder();
    query.select({{"id", {}}, {"tenant_id", {}}}).where(projection_detail::equal(expressions, "tenant_id", tenant));
    const auto rows = co_await query.get_many<projection_node_index>();
    std::set<std::string> nodes;
    for (const auto& row : rows) {
        nodes.emplace(row.get<"id">());
    }
    for (const auto& id : projection_detail::members(co_await redis.command("SMEMBERS", tenant_nodes_key(tenant)))) {
        nodes.emplace(id);
    }
    for (const auto& id : nodes) {
        co_await projection_detail::refresh_node(transaction, redis, tenant, id);
    }
    co_await projection_detail::clean_tenant_index(redis, tenant);
}

template <typename context_type>
ruvia::task<void> refresh(context_type& context, std::optional<std::string_view> tenant = {}) {
    auto redis = context.redis().with_options({.timeout_ = projection_command_timeout,
        .stop_token_ = context.get_stop_token()});
    std::map<std::string, std::set<std::string>> nodes;
    ruvia::db_query expressions;
    auto query = context.db().template get_repository<projection_node_entity>().create_query_builder();
    query.select({{"id", {}}, {"tenant_id", {}}});
    if (tenant) {
        query.where(projection_detail::equal(expressions, "tenant_id", *tenant));
    }
    const auto rows = co_await query.template get_many<projection_node_index>();
    for (const auto& row : rows) {
        nodes[std::string(row.template get<"tenant_id">())].emplace(row.template get<"id">());
    }
    if (tenant) {
        nodes[std::string(*tenant)];
    } else {
        for (const auto& id : projection_detail::members(co_await redis.command("SMEMBERS", tenants_key))) {
            nodes[id];
        }
    }
    for (auto& [tenant_id, node_ids] : nodes) {
        for (const auto& id : projection_detail::members(co_await redis.command("SMEMBERS", tenant_nodes_key(tenant_id)))) {
            node_ids.emplace(id);
        }
        for (const auto& node_id : node_ids) {
            co_await projection_detail::refresh_node(context, tenant_id, node_id);
        }
        co_await projection_detail::clean_tenant_index(redis, tenant_id);
    }
    // Recovery repairs partial Redis loss as well: an obsolete credential
    // cannot survive merely because its reverse index was evicted separately.
    if (!tenant) {
        co_await projection_detail::prune_orphan_agents(context);
    }
}

// Committed resource notifications that change node desired state. The
// outbox runs this in its claim transaction before publishing them, so the
// connections they wake read the refreshed projection. Rows of a tenant whose
// projection could not be refreshed stay durable for the outbox retry.
struct projection_refresh final {
    template <typename context_type, typename rows_type>
    static ruvia::task<std::set<std::string>> prepare(ruvia::db_transaction& transaction, context_type& context, const rows_type& rows) {
        namespace live = service::live_resource;
        // Ordered by tenant: every claimant acquires node locks in the same order.
        std::map<std::string, std::vector<std::string>> tenants;
        for (const auto& row : rows) {
            if (std::string_view(row.template get<"kind">()) != live::outbox::resource_kind) {
                continue;
            }
            try {
                const auto notification = live::notifications::decode(live::outbox::unpack(row.template get<"payload">()));
                if (notification && notification->node_config_) {
                    tenants[notification->tenant_id_].emplace_back(row.template get<"id">());
                }
            } catch (const std::runtime_error&) {
                // The relay rejects the malformed row; it is not a projection input.
            }
        }
        std::set<std::string> deferred;
        for (const auto& [tenant, ids] : tenants) {
            try {
                co_await refresh(transaction, context.redis(), tenant);
            } catch (const ruvia::db_error&) {
                throw;
            } catch (const std::exception&) {
                deferred.insert(ids.begin(), ids.end());
            }
        }
        co_return deferred;
    }
};

inline ruvia::task<std::string> repair_generation(ruvia::redis_handle redis) {
    const auto token = service::utils::random_token();
    static constexpr std::string_view script = R"lua(
local current = redis.call('GET', KEYS[1])
if not current then redis.call('SET', KEYS[1], ARGV[1]); return ARGV[1] end
return current
)lua";
    const std::array keys{repair_key};
    const std::array<std::string_view, 1> arguments{token};
    const auto result = co_await redis.eval(script, keys, arguments);
    service::utils::require_redis_success(result, "could not begin node projection repair");
    co_return std::string(result.string());
}

inline ruvia::task<bool> complete_repair(ruvia::redis_handle redis, std::string_view generation) {
    static constexpr std::string_view script = R"lua(
if redis.call('GET', KEYS[2]) ~= ARGV[1] then return 0 end
if redis.call('GET', KEYS[1]) == ARGV[1] then return 1 end
local kind = redis.call('TYPE', KEYS[3]).ok
if kind ~= 'none' and kind ~= 'stream' then error('invalid resource recovery stream') end
local clock = redis.call('TIME')
local now = tonumber(clock[1]) * 1000 + math.floor(tonumber(clock[2]) / 1000)
redis.call('XADD', KEYS[3], 'MAXLEN', '~', ARGV[2], '*', 'tenant_id', '*', 'missed_until', tostring(now), 'event_id', 'projection:' .. ARGV[1])
redis.call('SET', KEYS[1], ARGV[1])
return 1
)lua";
    const std::array keys{ready_key, repair_key, service::live_resource::notifications::stream_key};
    const auto maximum = std::to_string(service::live_resource::notifications::max_entries);
    const std::array<std::string_view, 2> arguments{generation, maximum};
    const auto result = co_await redis.eval(script, keys, arguments);
    service::utils::require_redis_success(result, "could not finish node projection repair");
    co_return result.integer() == 1;
}

template <typename context_type>
ruvia::task<void> run_projection(context_type& context) {
    const auto worker = context.worker();
    bool startup{true};
    while (!context.get_stop_token().stop_requested()) {
        try {
            auto redis = context.redis().with_options({.timeout_ = projection_command_timeout,
                .stop_token_ = context.get_stop_token()});
            const auto ready = co_await redis.command("MGET", ready_key, repair_key);
            service::utils::require_redis_success(ready, "could not read node projection readiness");
            if (ready.kind() != ruvia::redis_value::kind_type::array || ready.array().size() != 2) {
                throw std::runtime_error("invalid node projection readiness reply");
            }
            if (startup || ready.array()[0].null() || ready.array()[1].null() ||
                ready.array()[0].string() != ready.array()[1].string()) {
                const auto generation = co_await repair_generation(redis);
                co_await refresh(context);
                (void)co_await complete_repair(redis, generation);
                startup = false;
            }
        } catch (const std::exception&) {
            if (context.get_stop_token().stop_requested()) {
                break;
            }
            service::logging::error("Node channel projection will retry Redis recovery");
        }
        if (co_await ruvia::sleep_for(worker, std::chrono::seconds(15), context.get_stop_token()) ==
            ruvia::timer_sleep_result::stop_requested) {
            break;
        }
    }
}

}  // namespace service::node_channel
