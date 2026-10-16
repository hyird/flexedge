#include <ruvia/core/event_loop_pool.h>
#include <ruvia/core/task_scope.h>
#include <ruvia/web/db/db_client.h>
#include <ruvia/web/redis/redis_client.h>

#include <chrono>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "service/features/node_channel/channel_service.h"
#include "service/features/node_channel/projection.h"
#include "support/postgres_test.h"
#include "support/redis_test.h"

namespace {

namespace channel = service::node_channel;

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error("node channel projection: " + std::string(message));
    }
}

std::string uuid() {
    auto value = service::utils::random_token().substr(0, 32);
    value.insert(20, "-");
    value.insert(16, "-");
    value.insert(12, "-");
    value.insert(8, "-");
    return value;
}

struct projection_context final {
    ruvia::db_client& database_;
    ruvia::redis_client& redis_;
    ruvia::stop_source stop_;
    std::size_t calls_{};

    ruvia::db_handle db() {
        ++calls_;
        return database_.with_options({.stop_token_ = get_stop_token()});
    }
    ruvia::redis_handle redis() const {
        return redis_.handle();
    }
    const ruvia::worker_handle& worker() const {
        return database_.worker();
    }
    ruvia::stop_token get_stop_token() const {
        return stop_.token();
    }
};

// This connection fixture deliberately has no database capability.
struct connection_context final {
    ruvia::redis_client& redis_;
    const ruvia::worker_handle& worker_;
    ruvia::stop_source stop_;
    service::node_dispatch::fanout::hub configuration_changes_;
    service::node_channel::report_waits report_waits_;
    service::background::work_available::claim_signals claims_;

    ruvia::redis_handle redis() const {
        return redis_.handle().with_options({.stop_token_ = get_stop_token()});
    }
    const ruvia::worker_handle& worker() const {
        return worker_;
    }
    ruvia::stop_token get_stop_token() const {
        return stop_.token();
    }
    template <typename state_type>
    state_type& worker_state() {
        if constexpr (std::is_same_v<state_type, service::node_dispatch::fanout::hub>) {
            return configuration_changes_;
        } else if constexpr (std::is_same_v<state_type, service::background::work_available::claim_signals>) {
            return claims_;
        } else {
            static_assert(std::is_same_v<state_type, service::node_channel::report_waits>);
            return report_waits_;
        }
    }
};

flexedge::node::v2::NodeSpecContent spec_content(const flexedge::node::v2::DesiredState& state) {
    auto content = flexedge::node::open_artifact<flexedge::node::v2::NodeSpecContent>(
        state.node_spec().digest_sha256(), state.node_spec().content());
    require(content.has_value(), "projected node spec content matches its digest");
    return std::move(*content);
}

flexedge::node::v2::ClusterReleaseContent release_content(const flexedge::node::v2::DesiredState& state) {
    auto content = flexedge::node::open_artifact<flexedge::node::v2::ClusterReleaseContent>(
        state.release().digest_sha256(), state.release().content());
    require(content.has_value(), "projected release content matches its digest");
    return std::move(*content);
}

ruvia::task<void> collect_desired(connection_context& context,
    const service::node_channel::agent_principal& principal,
    std::optional<flexedge::node::v2::DesiredState>& result, std::exception_ptr& failure) {
    try {
        result = (co_await service::node_channel::get_channel_service().desired_state(context, principal)).state_;
    } catch (...) {
        failure = std::current_exception();
    }
}

ruvia::task<void> prepare_repair(ruvia::redis_handle redis) {
    const auto generation = service::utils::random_token();
    (void)co_await redis.set(channel::repair_key, generation);
    (void)co_await redis.set(channel::ready_key, generation);
    (void)co_await redis.del(channel::repair_cooldown_key);
}

ruvia::task<std::string> read_ready(ruvia::redis_handle redis) {
    const auto value = co_await redis.command("GET", channel::ready_key);
    co_return value.null() ? std::string{} : std::string(value.string());
}

ruvia::task<flexedge::node::v2::DesiredState> read_desired(ruvia::redis_handle redis,
    const service::node_channel::agent_principal& principal) {
    const auto value = co_await channel::desired(redis, principal);
    require(value.has_value(), "registered node has its desired state");
    const auto plaintext = service::utils::open_secret(*value);
    flexedge::node::v2::DesiredState result;
    require(flexedge::node::parse_artifact(plaintext, result), "desired state is a sealed protobuf");
    co_return result;
}

ruvia::task<void> verify(ruvia::db_client& first, ruvia::db_client& second,
    ruvia::redis_client& redis, ruvia::redis_client& peer) {
    co_await first.connect();
    co_await second.connect();
    co_await redis.connect();
    co_await peer.connect();
    auto db = first.handle();
    auto peer_db = second.handle();
    auto cache = redis.handle();
    const auto schema = "projection_qa_" + service::utils::random_token().substr(0, 16);
    const auto tenant = uuid(), other_tenant = uuid(), node = uuid(), other_node = uuid();
    const auto cluster = uuid(), release = uuid(), agent = service::utils::random_token().substr(0, 32);
    const auto other_agent = service::utils::random_token().substr(0, 32);
    const auto rotated_agent = service::utils::random_token().substr(0, 32);
    std::vector<std::string> agents{agent, other_agent, rotated_agent};
    projection_context context{.database_ = first, .redis_ = redis, .stop_ = {}};
    connection_context connection{.redis_ = peer, .worker_ = first.worker(), .stop_ = {}, .configuration_changes_ = {}, .report_waits_ = {}, .claims_ = {}};
    (void)co_await db.execute("CREATE SCHEMA " + schema);
    std::exception_ptr failure;
    try {
        (void)co_await db.execute("SET search_path TO " + schema);
        (void)co_await peer_db.execute("SET search_path TO " + schema);
        // A narrow fixture with the physical types consumed by the read-model
        // entities. It intentionally has no parallel notification or API flow.
        const std::array fixture{
            R"sql(CREATE TABLE sys_node (
 id uuid PRIMARY KEY, tenant_id uuid NOT NULL, cluster_id uuid NOT NULL,
 agent_id varchar(32), node_secret_hash char(64), registration_status varchar(16) NOT NULL,
 name varchar(100) NOT NULL, status varchar(16) NOT NULL, node_spec_revision bigint NOT NULL,
 node_spec_digest char(64), config jsonb NOT NULL, desired_release_id uuid, deleted_at timestamptz))sql",
            R"sql(CREATE TABLE sys_cluster_release (
 tenant_id uuid NOT NULL, id uuid NOT NULL, cluster_id uuid NOT NULL,
 manifest_digest char(64), manifest_envelope text, PRIMARY KEY(tenant_id,id)))sql",
            R"sql(CREATE TABLE sys_delivery_object (
 tenant_id uuid NOT NULL, digest_sha256 char(64) NOT NULL, kind varchar(16) NOT NULL,
 payload_envelope text NOT NULL, PRIMARY KEY(tenant_id,digest_sha256)))sql",
            R"sql(CREATE TABLE sys_cluster_release_object (
 tenant_id uuid NOT NULL, release_id uuid NOT NULL, position integer NOT NULL,
 object_digest char(64) NOT NULL, PRIMARY KEY(tenant_id,release_id,position)))sql"};
        for (const auto* statement : fixture) {
            (void)co_await db.execute(statement);
        }
        const auto secret_hash = std::string(64, 'a');
        const auto insert_node = R"sql(INSERT INTO sys_node
 (id,tenant_id,cluster_id,agent_id,node_secret_hash,registration_status,name,status,node_spec_revision,config)
 VALUES($1,$2,$3,$4,$5,'pending','projection node','enabled',1,'{"endpoints":[]}'::jsonb))sql";
        (void)co_await db.execute(insert_node, std::vector<ruvia::db_value>{ruvia::db_value{node}, ruvia::db_value{tenant}, ruvia::db_value{cluster}, ruvia::db_value{agent}, ruvia::db_value{secret_hash}});
        (void)co_await db.execute(insert_node, std::vector<ruvia::db_value>{ruvia::db_value{other_node}, ruvia::db_value{other_tenant}, ruvia::db_value{cluster}, ruvia::db_value{other_agent}, ruvia::db_value{secret_hash}});
        co_await channel::refresh(context, tenant);
        const auto pending = co_await channel::identity(peer.handle(), agent);
        require(pending && pending->tenant_id_ == tenant && pending->node_id_ == node && pending->status_ == "pending", "pending authentication identity crosses clients");
        require((co_await cache.command("TTL", channel::agent_key(agent))).integer() == -1 &&
                    (co_await cache.command("TTL", channel::node_index_key(tenant, node))).integer() == -1,
            "materialized identity and reverse index have no query-triggering TTL");
        require(!(co_await channel::identity(peer.handle(), other_agent)), "tenant refresh does not project another tenant");
        const service::node_channel::agent_principal principal{node, cluster, tenant, agent};
        require(!(co_await channel::desired(peer.handle(), principal)), "pending identity has no desired configuration");

        flexedge::node::v2::DeliveryObjectContent object;
        object.mutable_certificate()->set_id(uuid());
        object.mutable_certificate()->set_private_key_pem("projection-private-material");
        const auto digest = flexedge::node::artifact_digest(flexedge::node::serialize_artifact(object));
        const auto envelope = service::utils::seal_secret(flexedge::node::serialize_artifact(object));
        flexedge::node::v2::ClusterReleaseContent content;
        content.set_release_id(release);
        content.set_cluster_id(cluster);
        content.set_generation(1);
        auto* reference = content.add_objects();
        reference->set_kind(flexedge::node::v2::OBJECT_KIND_CERTIFICATE);
        reference->set_digest_sha256(digest);
        const auto manifest = flexedge::node::pack_artifact<flexedge::node::v2::ClusterReleaseManifest>(content);
        const auto manifest_envelope = service::utils::seal_secret(flexedge::node::serialize_artifact(manifest));
        (void)co_await db.execute("INSERT INTO sys_cluster_release VALUES($1,$2,$3,$4,$5)",
            std::vector<ruvia::db_value>{ruvia::db_value{tenant}, ruvia::db_value{release}, ruvia::db_value{cluster}, ruvia::db_value{manifest.digest_sha256()}, ruvia::db_value{manifest_envelope}});
        (void)co_await db.execute("INSERT INTO sys_delivery_object VALUES($1,$2,'certificate',$3)",
            std::vector<ruvia::db_value>{ruvia::db_value{tenant}, ruvia::db_value{digest}, ruvia::db_value{envelope}});
        (void)co_await db.execute("INSERT INTO sys_cluster_release_object VALUES($1,$2,0,$3)",
            std::vector<ruvia::db_value>{ruvia::db_value{tenant}, ruvia::db_value{release}, ruvia::db_value{digest}});
        (void)co_await db.execute("UPDATE sys_node SET registration_status='registered',desired_release_id=$1 WHERE id=$2",
            std::vector<ruvia::db_value>{ruvia::db_value{release}, ruvia::db_value{node}});
        auto delivery = co_await db.begin_transaction();
        co_await channel::refresh(delivery, cache, tenant);
        co_await delivery.rollback();
        auto state = co_await read_desired(peer.handle(), principal);
        require(spec_content(state).revision() == 1, "single-connection delivery reuses its outer transaction without waiting for another lease");
        const auto rolled_back_digest = co_await db.query("SELECT node_spec_digest IS NULL FROM sys_node WHERE id=$1",
            std::vector<ruvia::db_value>{ruvia::db_value{node}});
        require(rolled_back_digest[0][0].as<bool>().value(), "outer rollback may leave a valid Redis projection but rolls back its derived SQL digest");
        co_await channel::refresh(context, tenant);
        state = co_await read_desired(peer.handle(), principal);
        require(spec_content(state).node_id() == node && spec_content(state).revision() == 1 &&
                    state.release().digest_sha256() == manifest.digest_sha256(),
            "registered projection binds node revision and validated release");
        const auto recorded = co_await db.query("SELECT node_spec_digest FROM sys_node WHERE id=$1", std::vector<ruvia::db_value>{ruvia::db_value{node}});
        require(recorded[0][0].as<std::string>() == state.node_spec().digest_sha256(), "projected node spec digest is persisted while the node is locked");
        const std::vector<std::string> request{digest};
        const auto projected = co_await channel::objects(peer.handle(), principal, release, request);
        require(projected.size() == 1 && projected[0] == envelope && projected[0].find("projection-private-material") == std::string::npos,
            "objects retain only their sealed validated payloads");
        auto wrong_tenant = principal;
        wrong_tenant.tenant_id_ = other_tenant;
        require(!(co_await channel::desired(peer.handle(), wrong_tenant)), "desired read checks the principal tenant");
        bool rejected{};
        try {
            (void)co_await channel::objects(peer.handle(), wrong_tenant, release, request);
        } catch (const std::exception&) {
            rejected = true;
        }
        require(rejected, "object read checks the principal tenant");
        rejected = false;
        try {
            (void)co_await channel::objects(peer.handle(), principal, uuid(), request);
        } catch (const std::exception&) {
            rejected = true;
        }
        require(rejected, "objects cannot read a release other than this node's current release");
        rejected = false;
        const std::vector<std::string> duplicate{digest, digest};
        try {
            (void)co_await channel::objects(peer.handle(), principal, release, duplicate);
        } catch (const std::exception&) {
            rejected = true;
        }
        require(rejected, "duplicate object digests are rejected");

        const auto& service = service::node_channel::get_channel_service();
        co_await prepare_repair(cache);
        const auto ready_generation = co_await read_ready(cache);
        for (const auto& id : {service::utils::random_token().substr(0, 32), agent}) {
            rejected = false;
            try {
                (void)co_await service.authenticate(connection, id, "wrong secret");
            } catch (const std::exception&) {
                rejected = true;
            }
            require(rejected, "unknown agents and incorrect credentials remain unauthorized");
        }
        require((co_await read_ready(cache)) == ready_generation &&
                    (co_await cache.command("EXISTS", channel::repair_cooldown_key)).integer() == 0,
            "unknown agents and incorrect secrets cannot request a database rebuild");
        for (const auto& requested_digest : {std::string("invalid"), std::string(64, '0')}) {
            google::protobuf::RepeatedPtrField<std::string> invalid_request;
            *invalid_request.Add() = requested_digest;
            rejected = false;
            try {
                (void)co_await service.objects(connection, principal, release, invalid_request);
            } catch (const std::exception&) {
                rejected = true;
            }
            require(rejected, "malformed digests and digests outside the current manifest are rejected");
        }
        google::protobuf::RepeatedPtrField<std::string> wrong_release_request;
        *wrong_release_request.Add() = digest;
        rejected = false;
        try {
            (void)co_await service.objects(connection, principal, uuid(), wrong_release_request);
        } catch (const std::exception&) {
            rejected = true;
        }
        require(rejected, "a request for another release is rejected without requesting repair");
        require((co_await read_ready(cache)) == ready_generation &&
                    (co_await cache.command("EXISTS", channel::repair_cooldown_key)).integer() == 0,
            "invalid object requests cannot request a database rebuild");

        // A broken desired payload wakes the original connection only after
        // a real PostgreSQL-to-Redis repair and its configuration notification.
        for (const bool missing : {true, false}) {
            co_await prepare_repair(cache);
            if (missing) {
                (void)co_await cache.command("HDEL", channel::agent_key(agent), "desired");
            } else {
                (void)co_await cache.command("HSET", channel::agent_key(agent), "desired", "corrupt sealed state");
            }
            std::optional<flexedge::node::v2::DesiredState> recovered_state;
            std::exception_ptr recovery_failure;
            ruvia::task_scope connection_tasks(first.worker());
            connection_tasks.spawn(collect_desired(connection, principal, recovered_state, recovery_failure));
            bool requested_repair{};
            for (int attempt = 0; attempt != 100; ++attempt) {
                if ((co_await cache.command("GET", channel::ready_key)).null()) {
                    requested_repair = true;
                    break;
                }
                (void)co_await ruvia::sleep_for(first.worker(), std::chrono::milliseconds(10));
            }
            co_await channel::refresh(context, tenant);
            const auto generation = co_await channel::repair_generation(cache);
            (void)co_await channel::complete_repair(cache, generation);
            connection.configuration_changes_.publish(tenant);
            co_await connection_tasks.join();
            if (recovery_failure) {
                std::rethrow_exception(recovery_failure);
            }
            require(requested_repair && recovered_state && release_content(*recovered_state).release_id() == release,
                "missing and corrupt desired payloads request repair and resume the same Redis-only connection");
        }

        google::protobuf::RepeatedPtrField<std::string> object_request;
        *object_request.Add() = digest;
        for (const bool missing : {true, false}) {
            co_await prepare_repair(cache);
            const auto field = "object:" + digest;
            if (missing) {
                (void)co_await cache.command("HDEL", channel::agent_key(agent), field);
            } else {
                (void)co_await cache.command("HSET", channel::agent_key(agent), field, "corrupt sealed object");
            }
            rejected = false;
            try {
                (void)co_await service.objects(connection, principal, release, object_request);
            } catch (const std::exception&) {
                rejected = true;
            }
            require(rejected && (co_await cache.command("GET", channel::ready_key)).null(),
                "missing and corrupt manifest objects fail closed and request repair");
            const auto generation = co_await channel::repair_generation(cache);
            for (int repeat = 0; repeat != 3; ++repeat) {
                require(co_await channel::request_repair(peer.handle(), principal, release), "pending repairs coalesce across clients");
                require((co_await channel::repair_generation(cache)) == generation, "repeated hints preserve the in-flight generation");
            }
            co_await channel::refresh(context, tenant);
            (void)co_await channel::complete_repair(cache, generation);
            require((co_await service.objects(connection, principal, release, object_request)).objects_size() == 1,
                "the next object request reads the repaired immutable content");
            require(!(co_await channel::request_repair(peer.handle(), principal, release)) &&
                        (co_await read_ready(cache)) == generation,
                "the Redis cooldown prevents another completed rebuild within sixty seconds");
            const auto remaining_cooldown = (co_await cache.command("PTTL", channel::repair_cooldown_key)).integer();
            require(remaining_cooldown > 0 && remaining_cooldown <= 60000, "repair cooldown is a bounded Redis lease");
            (void)co_await cache.command("PEXPIRE", channel::repair_cooldown_key, "1");
            (void)co_await ruvia::sleep_for(first.worker(), std::chrono::milliseconds(5));
            require(co_await channel::request_repair(peer.handle(), principal, release), "a new repair can be requested after the Redis cooldown expires");
            (void)co_await channel::complete_repair(cache, co_await channel::repair_generation(cache));
        }

        // A source writer holds the node lock. The projector must wait, then
        // build from its committed version rather than a previously read copy.
        const auto pid_rows = co_await db.query("SELECT pg_backend_pid()");
        const auto pid = pid_rows[0][0].as<std::int64_t>().value();
        auto writer = co_await peer_db.begin_transaction();
        (void)co_await writer.query("SELECT id FROM sys_node WHERE id=$1 FOR UPDATE", std::vector<ruvia::db_value>{ruvia::db_value{node}});
        ruvia::task_scope refresh_tasks(first.worker());
        refresh_tasks.spawn(channel::refresh(context, tenant));
        bool locked{};
        for (int attempt = 0; attempt != 100; ++attempt) {
            const auto waits = co_await writer.query("SELECT wait_event_type='Lock' FROM pg_stat_activity WHERE pid=$1", std::vector<ruvia::db_value>{ruvia::db_value{pid}});
            if (waits[0][0].as<bool>().value_or(false)) {
                locked = true;
                break;
            }
            (void)co_await ruvia::sleep_for(first.worker(), std::chrono::milliseconds(10));
        }
        (void)co_await writer.execute("UPDATE sys_node SET node_spec_revision=2,name='committed new name' WHERE id=$1", std::vector<ruvia::db_value>{ruvia::db_value{node}});
        co_await writer.commit();
        co_await refresh_tasks.join();
        require(locked, "projector uses a real PostgreSQL row lock");
        state = co_await read_desired(peer.handle(), principal);
        require(spec_content(state).revision() == 2 && spec_content(state).name() == "committed new name", "lock acquisition reads the committed source version");

        // Invalid immutable artifacts fail the transaction without replacing a
        // previously usable projection or persisting a speculative spec digest.
        const auto invalid_envelope = service::utils::seal_secret("invalid protobuf");
        (void)co_await db.execute("UPDATE sys_delivery_object SET payload_envelope=$1 WHERE tenant_id=$2 AND digest_sha256=$3",
            std::vector<ruvia::db_value>{ruvia::db_value{invalid_envelope}, ruvia::db_value{tenant}, ruvia::db_value{digest}});
        (void)co_await db.execute("UPDATE sys_node SET node_spec_revision=3 WHERE id=$1", std::vector<ruvia::db_value>{ruvia::db_value{node}});
        rejected = false;
        try {
            co_await channel::refresh(context, tenant);
        } catch (const std::exception&) {
            rejected = true;
        }
        require(rejected, "invalid immutable object fails refresh");
        // The framework closes an abandoned transaction's connection to roll
        // it back; reconnecting has the default search_path again.
        (void)co_await db.execute("SET search_path TO " + schema);
        state = co_await read_desired(peer.handle(), principal);
        require(spec_content(state).revision() == 2, "failed refresh keeps its last complete Redis record");
        const auto failed_digest = co_await db.query("SELECT node_spec_digest FROM sys_node WHERE id=$1", std::vector<ruvia::db_value>{ruvia::db_value{node}});
        require(failed_digest[0][0].as<std::string>() == state.node_spec().digest_sha256(), "failed artifact validation rolls back the spec digest update");
        (void)co_await db.execute("UPDATE sys_delivery_object SET payload_envelope=$1 WHERE tenant_id=$2 AND digest_sha256=$3",
            std::vector<ruvia::db_value>{ruvia::db_value{envelope}, ruvia::db_value{tenant}, ruvia::db_value{digest}});
        co_await channel::refresh(context, tenant);

        (void)co_await db.execute("UPDATE sys_node SET agent_id=$1,registration_status='pending',desired_release_id=NULL WHERE id=$2",
            std::vector<ruvia::db_value>{ruvia::db_value{rotated_agent}, ruvia::db_value{node}});
        co_await channel::refresh(context, tenant);
        require(!(co_await channel::identity(peer.handle(), agent)), "credential rotation removes the previous identity immediately");
        require((co_await cache.command("HGET", channel::identities_key, agent)).null() &&
                    !(co_await channel::request_repair(peer.handle(), principal)),
            "a rotated credential loses its repair proof as well as its authentication payload");
        require((co_await channel::identity(peer.handle(), rotated_agent)).has_value(), "rotated pending identity is published");
        require(!(co_await channel::desired(peer.handle(), principal)), "a previously authenticated principal cannot read after rotation");
        (void)co_await db.execute("UPDATE sys_node SET deleted_at=NOW() WHERE id=$1", std::vector<ruvia::db_value>{ruvia::db_value{node}});
        co_await channel::refresh(context, tenant);
        require(!(co_await channel::identity(peer.handle(), rotated_agent)), "soft deletion removes authentication and configuration bytes");
        require((co_await cache.command("HGET", channel::identities_key, rotated_agent)).null(), "soft deletion revokes its independent repair proof");
        co_await channel::refresh(context, other_tenant);
        (void)co_await db.execute("DELETE FROM sys_node WHERE id=$1", std::vector<ruvia::db_value>{ruvia::db_value{other_node}});
        co_await channel::refresh(context, other_tenant);
        require(!(co_await channel::identity(peer.handle(), other_agent)), "tenant index also removes physically deleted nodes");

        (void)co_await db.execute(insert_node,
            std::vector<ruvia::db_value>{ruvia::db_value{other_node}, ruvia::db_value{other_tenant}, ruvia::db_value{cluster}, ruvia::db_value{other_agent}, ruvia::db_value{secret_hash}});
        co_await channel::refresh(context, other_tenant);
        // Lose both reverse indexes before source deletion, leaving only an
        // old identity HASH. A full repair must still revoke that credential.
        (void)co_await cache.del(channel::node_index_key(other_tenant, other_node));
        (void)co_await cache.del(channel::tenant_nodes_key(other_tenant));
        (void)co_await cache.command("SREM", channel::tenants_key, other_tenant);
        (void)co_await db.execute("DELETE FROM sys_node WHERE id=$1", std::vector<ruvia::db_value>{ruvia::db_value{other_node}});
        co_await channel::refresh(context);
        require(!(co_await channel::identity(peer.handle(), other_agent)), "full repair removes obsolete identities despite lost reverse indexes");
        require((co_await cache.command("HGET", channel::identities_key, other_agent)).null(), "full repair removes orphaned repair proofs");
        (void)co_await db.execute(insert_node,
            std::vector<ruvia::db_value>{ruvia::db_value{other_node}, ruvia::db_value{other_tenant}, ruvia::db_value{cluster}, ruvia::db_value{other_agent}, ruvia::db_value{secret_hash}});
        co_await channel::refresh(context, other_tenant);
        (void)co_await cache.command("DEL", channel::agent_key(other_agent), channel::node_index_key(other_tenant, other_node), channel::tenant_nodes_key(other_tenant));
        (void)co_await cache.command("SREM", channel::tenants_key, other_tenant);
        (void)co_await db.execute("DELETE FROM sys_node WHERE id=$1", std::vector<ruvia::db_value>{ruvia::db_value{other_node}});
        co_await channel::refresh(context);
        require((co_await cache.command("HGET", channel::identities_key, other_agent)).null() &&
                    !(co_await channel::request_repair(peer.handle(), other_agent)),
            "orphan repair proofs are revoked even when every payload and reverse index was lost");

        // Generation replacement cannot be acknowledged by an older full
        // rebuild, including Redis resets while a rebuild is in flight.
        co_await prepare_repair(cache);
        (void)co_await cache.del(channel::ready_key);
        const auto old_generation = co_await channel::repair_generation(cache);
        const auto notification_count = (co_await cache.command("XLEN", service::live_resource::notifications::stream_key)).integer();
        require(co_await channel::complete_repair(cache, old_generation), "completed generation becomes ready");
        require(co_await channel::complete_repair(peer.handle(), old_generation), "another worker can finish the same generation");
        require((co_await cache.command("XLEN", service::live_resource::notifications::stream_key)).integer() == notification_count + 1,
            "concurrent generation completion emits one cross-worker recovery hint");
        co_await prepare_repair(peer.handle());
        (void)co_await cache.del(channel::ready_key);
        const auto new_generation = co_await channel::repair_generation(cache);
        require(old_generation != new_generation && !(co_await channel::complete_repair(cache, old_generation)), "older rebuild cannot cover a newer repair request");
        (void)co_await cache.del(channel::repair_key);
        require(!(co_await channel::complete_repair(cache, new_generation)), "reset during rebuild rejects its old completion");

        // Bring back one live source and lose its Redis bytes. Every worker
        // follows the same repair path; idle checks query Redis, not sys_node.
        (void)co_await db.execute("UPDATE sys_node SET deleted_at=NULL WHERE id=$1", std::vector<ruvia::db_value>{ruvia::db_value{node}});
        (void)co_await cache.del(channel::ready_key);
        (void)co_await cache.del(channel::repair_cooldown_key);
        projection_context running{.database_ = first, .redis_ = redis, .stop_ = {}};
        ruvia::task_scope runtime(first.worker());
        runtime.spawn(channel::run_projection(running));
        std::exception_ptr runtime_failure;
        try {
            bool ready{};
            for (int attempt = 0; attempt != 100; ++attempt) {
                if (!(co_await cache.command("GET", channel::ready_key)).null()) {
                    ready = true;
                    break;
                }
                (void)co_await ruvia::sleep_for(first.worker(), std::chrono::milliseconds(10));
            }
            require(ready && (co_await channel::identity(peer.handle(), rotated_agent)).has_value(), "startup rebuild restores missing Redis state");
            const auto idle_calls = running.calls_;
            (void)co_await ruvia::sleep_for(first.worker(), std::chrono::seconds(16));
            require(running.calls_ == idle_calls, "idle readiness interval performs zero business database queries");
            (void)co_await cache.del(channel::agent_key(rotated_agent));
            rejected = false;
            try {
                (void)co_await service.authenticate(connection, rotated_agent, "unverified secret");
            } catch (const std::exception&) {
                rejected = true;
            }
            require(rejected && (co_await cache.command("GET", channel::ready_key)).null(),
                "a known agent with a missing HASH requests repair but cannot authenticate");
            bool recovered{};
            for (int attempt = 0; attempt != 170; ++attempt) {
                if ((co_await channel::identity(peer.handle(), rotated_agent)).has_value()) {
                    recovered = true;
                    break;
                }
                (void)co_await ruvia::sleep_for(first.worker(), std::chrono::milliseconds(100));
            }
            require(recovered && running.calls_ > idle_calls, "the connection's repair hint alone triggers a new database rebuild");
        } catch (...) {
            runtime_failure = std::current_exception();
        }
        const auto stopped_at = std::chrono::steady_clock::now();
        running.stop_.request_stop();
        co_await runtime.join();
        if (runtime_failure) {
            std::rethrow_exception(runtime_failure);
        }
        require(std::chrono::steady_clock::now() - stopped_at < std::chrono::milliseconds(500), "projection cancellation joins its timer and owned operations");
        std::cout << "Redis-only identity/desired/objects, tenant boundaries, sealed artifacts, row locks, rollback, rotation/deletion, missing and corrupt payload repair, unknown/digest rejection, cooldown expiry, repair fencing, idle zero-DB and cancellation passed\n";
    } catch (...) {
        failure = std::current_exception();
    }
    (void)co_await db.execute("SET search_path TO public");
    (void)co_await peer_db.execute("SET search_path TO public");
    (void)co_await db.execute("DROP SCHEMA " + schema + " CASCADE");
    for (const auto& id : agents) {
        (void)co_await cache.del(channel::agent_key(id));
        (void)co_await cache.command("HDEL", channel::identities_key, id);
    }
    for (const auto& id : {tenant, other_tenant}) {
        (void)co_await cache.del(channel::tenant_nodes_key(id));
        (void)co_await cache.command("SREM", channel::tenants_key, id);
    }
    (void)co_await cache.command("DEL", channel::node_index_key(tenant, node), channel::node_index_key(other_tenant, other_node));
    (void)co_await cache.command("DEL", channel::ready_key, channel::repair_key, channel::repair_cooldown_key);
    if (failure) {
        std::rethrow_exception(failure);
    }
}

}  // namespace

int main() {
    try {
        const auto postgres = test_support::isolated_postgres_config();
        const auto redis_config = test_support::isolated_redis_config();
        if (!postgres || !redis_config) {
            return 77;
        }
        const test_support::postgres_test_database temporary_database(*postgres);
        service::utils::configure_secret_key(std::string(64, '1'));
        ruvia::event_loop_pool loops({.loop_count_ = 1});
        const auto loop = loops.loop(0);
        ruvia::db_client first(loop, temporary_database.config()), second(loop, temporary_database.config());
        ruvia::redis_client redis(loop, *redis_config), peer(loop, *redis_config);
        loops.start();
        std::exception_ptr failure;
        try {
            loop.start(verify(first, second, redis, peer)).get();
        } catch (...) {
            failure = std::current_exception();
        }
        try {
            loop.start(first.shutdown()).get();
            loop.start(second.shutdown()).get();
        } catch (...) {
            if (!failure) {
                failure = std::current_exception();
            }
        }
        first.close();
        second.close();
        redis.close();
        peer.close();
        loops.stop();
        loops.join();
        if (failure) {
            std::rethrow_exception(failure);
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
