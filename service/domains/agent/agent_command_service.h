#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/context.h>
#include <ruvia/web/model_json.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

#include "node/proto/artifact.h"
#include "node/proto/edge_control.pb.h"
#include "service/common/http.h"
#include "service/domains/agent/agent_error.h"
#include "service/domains/agent/agent_runtime_mapper.h"
#include "service/domains/agent/agent_types.h"
#include "service/domains/agent/apply_result_store.h"
#include "service/domains/agent/desired_state_store.h"
#include "service/domains/agent/heartbeat_store.h"
#include "service/domains/agent/release_manifest.h"
#include "service/features/cluster_dns/projection.h"
#include "service/features/live_resource/node_runtime.h"
#include "service/features/node_dispatch/notifications.h"
#include "service/features/node_dispatch/protocol.h"
#include "service/features/node_dispatch/queue.h"
#include "service/features/node_dispatch/target_store.h"
#include "service/utils/secret.h"
#include "service/utils/sensitive_string.h"
#include "service/utils/token.h"

namespace service::agent {

class agent_command_service final {
public:
    ruvia::task<agent_principal> authenticate(ruvia::context& c, const std::string& agent_id,
        std::string_view secret) {
        auto transaction = co_await c.db().begin_transaction();
        const auto candidates = co_await transaction.query(
            "SELECT id, cluster_id, tenant_id, registration_status, node_secret_hash FROM "
            "sys_node WHERE agent_id = $1 AND deleted_at IS NULL LIMIT 1",
            agent_id);
        if (candidates.empty()) {
            service::common::throw_app_error(agent_unauthorized);
        }
        const auto stored_hash = candidates.front()[4].value();
        if (!stored_hash || !service::utils::token_hash_matches(secret, *stored_hash)) {
            service::common::throw_app_error(agent_unauthorized);
        }
        const auto initial_status = candidates.front()[3].value().value_or("");
        if (initial_status != "pending" && initial_status != "registered") {
            service::common::throw_app_error(agent_unauthorized);
        }
        const auto node_id = candidates.front()[0].value().value_or("");
        const auto cluster_id = candidates.front()[1].value().value_or("");
        const auto tenant_id = candidates.front()[2].value().value_or("");
        const auto rows = co_await transaction.query(
            "SELECT id, cluster_id, tenant_id, registration_status, node_secret_hash FROM "
            "sys_node WHERE id = $1 AND cluster_id = $2 AND tenant_id = $3 AND agent_id = $4 "
            "AND deleted_at IS NULL LIMIT 1 FOR UPDATE",
            node_id, cluster_id, tenant_id, agent_id);
        if (rows.empty()) {
            service::common::throw_app_error(agent_unauthorized);
        }
        const auto current_hash = rows.front()[4].value();
        if (!current_hash || !service::utils::token_hash_matches(secret, *current_hash)) {
            service::common::throw_app_error(agent_unauthorized);
        }
        if (rows.front()[3].value().value_or("") == "pending") {
            const auto updated = co_await transaction.execute(
                "UPDATE sys_node SET registration_status = 'registered', registered_at = NOW(), "
                "last_heartbeat_at = NOW(), desired_release_id = (SELECT current_release_id FROM "
                "sys_cluster WHERE tenant_id = sys_node.tenant_id AND id = "
                "sys_node.cluster_id), updated_at = NOW() WHERE id = $1 "
                "AND agent_id = $2 AND registration_status = 'pending' AND deleted_at IS NULL",
                rows.front()[0].value().value_or(""), agent_id);
            if (updated.affected_rows() != 1) {
                service::common::throw_app_error(agent_unauthorized);
            }
            // The target set is created from registered nodes. Publish only after this node has
            // transitioned out of pending so the initial release includes it as a target.
            co_await service::node_dispatch::publish_cluster_release(transaction, tenant_id,
                cluster_id);
            const std::string tenant_id_value(tenant_id);
            const std::string cluster_id_value(cluster_id);
            co_await service::cluster_dns::reconcile_cluster(transaction, tenant_id_value,
                cluster_id_value);
        } else if (rows.front()[3].value().value_or("") == "registered") {
            co_await service::node_dispatch::ensure_cluster_release(transaction, tenant_id, cluster_id);
        } else {
            service::common::throw_app_error(agent_unauthorized);
        }
        agent_principal result{
            .node_id_ = std::string(rows.front()[0].value().value_or("")),
            .cluster_id_ = std::string(rows.front()[1].value().value_or("")),
            .tenant_id_ = std::string(rows.front()[2].value().value_or("")),
            .agent_id_ = agent_id,
        };
        co_await transaction.commit();
        service::node_dispatch::notifications::published(result.tenant_id_);
        service::live_resource::get_hub().publish(result.tenant_id_, service::live_resource::resource::nodes, result.node_id_);
        service::live_resource::get_hub().publish(result.tenant_id_, service::live_resource::resource::clusters, result.cluster_id_);
        service::live_resource::get_hub().publish(result.tenant_id_, service::live_resource::resource::tasks);
        service::live_resource::get_node_deadlines().touch(result.tenant_id_, result.node_id_);
        co_return result;
    }

    ruvia::task<flexedge::node::v2::DesiredState> desired_state(ruvia::context& c,
        const agent_principal& principal) {
        const auto& node_id = principal.node_id_;
        auto transaction = co_await c.db().begin_transaction();
        const auto state = co_await lock_desired_state(transaction, principal);
        if (!state) {
            service::common::throw_app_error(revision_invalid);
        }
        flexedge::node::v2::DesiredState result;
        *result.mutable_node_spec() = service::node_dispatch::build_node_spec(
            node_id, state->revision_, state->name_, state->enabled_, state->config_json_);
        service::utils::sensitive_string manifest_bytes(service::utils::open_secret(state->manifest_envelope_));
        auto manifest = parse_release_manifest(manifest_bytes.view(), state->manifest_digest_,
            state->release_id_, principal.cluster_id_);
        if (!manifest) {
            service::common::throw_app_error(artifact_invalid);
        }
        *result.mutable_release() = std::move(*manifest);
        if (!(co_await record_node_spec_digest(transaction, principal.tenant_id_, node_id,
                state->revision_, result.node_spec().digest_sha256()))) {
            service::common::throw_app_error(revision_invalid);
        }
        co_await transaction.commit();
        co_return result;
    }

    ruvia::task<void> heartbeat(ruvia::context& c, const agent_principal& principal,
        const heartbeat_report& report) {
        if (report.node_id_ != principal.node_id_) {
            service::common::throw_app_error(agent_unauthorized);
        }
        const auto runtime_json = heartbeat_runtime_json(c, report);
        auto transaction = co_await c.db().begin_transaction();
        const auto update = co_await update_heartbeat(transaction, principal, report, runtime_json);
        if (!update) {
            service::common::throw_app_error(revision_invalid);
        }
        const auto target_changed = co_await service::node_dispatch::mark_node_target_applied(transaction, principal.tenant_id_, report.active_release_id_, report.node_id_);
        co_await transaction.commit();
        service::live_resource::get_node_deadlines().touch(principal.tenant_id_, principal.node_id_);
        service::live_resource::get_hub().publish_runtime(principal.tenant_id_, principal.node_id_,
            service::live_resource::runtime_patch(c, report, *update));
        service::live_resource::publish_origin_runtime(c, principal.tenant_id_, report, *update);
        if (update->became_online_) {
            service::live_resource::get_hub().publish(principal.tenant_id_, service::live_resource::resource::nodes, principal.node_id_);
            service::live_resource::get_hub().publish(principal.tenant_id_, service::live_resource::resource::overview);
            service::live_resource::get_hub().publish(principal.tenant_id_, service::live_resource::resource::clusters, principal.cluster_id_);
        }
        if (target_changed) {
            service::live_resource::get_hub().publish(principal.tenant_id_, service::live_resource::resource::tasks);
            service::live_resource::get_hub().publish(principal.tenant_id_, service::live_resource::resource::websites);
            service::live_resource::get_hub().publish(principal.tenant_id_, service::live_resource::resource::clusters, principal.cluster_id_);
        }
        co_return;
    }

    ruvia::task<void> record_apply_result(ruvia::context& c, const agent_principal& principal,
        const flexedge::node::v2::ApplyResult& result) {
        const auto& node_id = principal.node_id_;
        if (result.applied()) {
            auto transaction = co_await c.db().begin_transaction();
            if (!(co_await record_applied_deployment(transaction, principal, result.node_spec_revision(),
                    result.release_id(), result.manifest_digest()))) {
                service::common::throw_app_error(revision_invalid);
            }
            co_await service::node_dispatch::mark_node_target_applied(transaction, principal.tenant_id_, result.release_id(), node_id);
            co_await transaction.commit();
            publish_apply_change(principal);
            co_return;
        }
        const auto phase = flexedge::node::v2::ApplyPhase_Name(result.failed_phase());
        auto transaction = co_await c.db().begin_transaction();
        const apply_failure_report failure{.revision_ = result.node_spec_revision(),
            .release_id_ = result.release_id(),
            .manifest_digest_ = result.manifest_digest(),
            .phase_ = phase,
            .error_code_ = result.error_code(),
            .error_ = result.error().substr(0, 1000),
            .retryable_ = result.retryable()};
        const auto outcome = co_await record_failed_deployment(transaction, principal, failure);
        if (outcome == apply_failure_outcome::rejected) {
            service::common::throw_app_error(revision_invalid);
        }
        if (outcome == apply_failure_outcome::recorded) {
            co_await service::node_dispatch::mark_node_target_failed(transaction,
                principal.tenant_id_, failure.release_id_, node_id, principal.cluster_id_,
                {.phase_ = failure.phase_, .error_code_ = failure.error_code_, .error_ = failure.error_, .retryable_ = failure.retryable_});
        }
        co_await transaction.commit();
        publish_apply_change(principal);
        co_return;
    }

private:
    static void publish_apply_change(const agent_principal& principal) {
        using service::live_resource::resource;
        auto& hub = service::live_resource::get_hub();
        hub.publish(principal.tenant_id_, resource::nodes, principal.node_id_);
        hub.publish(principal.tenant_id_, resource::clusters, principal.cluster_id_);
        hub.publish(principal.tenant_id_, resource::websites);
        hub.publish(principal.tenant_id_, resource::tasks);
    }
};

inline agent_command_service& get_agent_command_service() {
    static agent_command_service service;
    return service;
}

}  // namespace service::agent
