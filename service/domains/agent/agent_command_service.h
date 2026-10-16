#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/model_json.h>

#include <chrono>
#include <cstdint>
#include <optional>
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
#include "service/features/attempt_trace/recorder.h"
#include "service/features/attempt_trace/trace_store.h"
#include "service/features/cluster_dns/projection.h"
#include "service/features/live_resource/node_runtime.h"
#include "service/features/live_resource/notification_outbox.h"
#include "service/features/node_dispatch/apply_history.h"
#include "service/features/node_dispatch/protocol.h"
#include "service/features/node_dispatch/queue.h"
#include "service/features/node_dispatch/target_store.h"
#include "service/utils/secret.h"
#include "service/utils/sensitive_string.h"
#include "service/utils/token.h"

namespace service::agent {

class agent_command_service final {
public:
    template <typename runtime_type>
    ruvia::task<agent_principal> authenticate(runtime_type& c, const std::string& agent_id,
        std::string_view secret) {
        auto transaction = co_await c.db().begin_transaction();
        const auto verified_hash = service::utils::token_hash(secret);
        co_return co_await authenticate_hashed(c, transaction, agent_id, verified_hash,
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(), true);
    }

    template <typename runtime_type>
    ruvia::task<agent_principal> authenticate_hashed(runtime_type& c, ruvia::db_transaction& transaction,
        const std::string& agent_id, std::string_view verified_hash, std::int64_t received_unix_ms,
        bool commit = false) {
        const auto candidates = co_await transaction.query(
            "SELECT id, cluster_id, tenant_id, registration_status, node_secret_hash FROM "
            "sys_node WHERE agent_id = $1 AND deleted_at IS NULL LIMIT 1",
            agent_id);
        if (candidates.empty()) {
            service::common::throw_app_error(agent_unauthorized);
        }
        const auto stored_hash = candidates.front()[4].value();
        if (!stored_hash || !hash_matches(verified_hash, *stored_hash)) {
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
        if (!current_hash || !hash_matches(verified_hash, *current_hash)) {
            service::common::throw_app_error(agent_unauthorized);
        }
        // Work an agent's authentication causes is a system task: the first
        // registration of a node (「节点接入」) or a release of the current
        // schema missing on reconnect (「配置格式升级」).
        auto origin = rows.front()[3].value().value_or("") == "pending"
                          ? service::operation::work_origin::system(
                                service::operation::action::system_node_register, "node", std::string(node_id))
                          : service::operation::work_origin::system(service::operation::action::system_release_upgrade,
                                "cluster", std::string(cluster_id));
        if (rows.front()[3].value().value_or("") == "pending") {
            const auto updated = co_await transaction.execute(
                "UPDATE sys_node SET registration_status = 'registered', registered_at = NOW(), "
                "last_heartbeat_at = to_timestamp($3::double precision / 1000.0), desired_release_id = (SELECT current_release_id FROM "
                "sys_cluster WHERE tenant_id = sys_node.tenant_id AND id = "
                "sys_node.cluster_id), updated_at = NOW() WHERE id = $1 "
                "AND agent_id = $2 AND registration_status = 'pending' AND deleted_at IS NULL",
                rows.front()[0].value().value_or(""), agent_id, received_unix_ms);
            if (updated.affected_rows() != 1) {
                service::common::throw_app_error(agent_unauthorized);
            }
            // The target set is created from registered nodes. Publish only after this node has
            // transitioned out of pending so the initial release includes it as a target.
            co_await service::node_dispatch::publish_cluster_release(transaction, tenant_id,
                cluster_id, origin);
            const std::string tenant_id_value(tenant_id);
            const std::string cluster_id_value(cluster_id);
            co_await service::cluster_dns::reconcile_cluster(transaction, tenant_id_value,
                cluster_id_value, origin);
        } else if (rows.front()[3].value().value_or("") == "registered") {
            co_await service::node_dispatch::ensure_cluster_release(transaction, tenant_id, cluster_id, origin);
        } else {
            service::common::throw_app_error(agent_unauthorized);
        }
        agent_principal result{
            .node_id_ = std::string(rows.front()[0].value().value_or("")),
            .cluster_id_ = std::string(rows.front()[1].value().value_or("")),
            .tenant_id_ = std::string(rows.front()[2].value().value_or("")),
            .agent_id_ = agent_id,
        };
        service::live_resource::notifications::resource_notification notification(result.tenant_id_);
        notification.node_config()
            .node_seen(result.node_id_)
            .changed(service::live_resource::resource::nodes, result.node_id_)
            .changed(service::live_resource::resource::clusters, result.cluster_id_);
        origin.add_changes(notification);
        notification.node_seen_unix_ms_ = received_unix_ms;
        if (commit) {
            co_await service::live_resource::commit_and_notify(transaction, c, notification);
        } else {
            (void)co_await service::live_resource::enqueue_notification(transaction, notification);
        }
        co_return result;
    }

    template <typename runtime_type>
    ruvia::task<flexedge::node::v2::DesiredState> desired_state(runtime_type& c,
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

    template <typename runtime_type>
    ruvia::task<void> heartbeat(runtime_type& c, const agent_principal& principal,
        const heartbeat_report& report) {
        auto transaction = co_await c.db().begin_transaction();
        co_await heartbeat(c, transaction, principal, report,
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(), true);
    }

    template <typename runtime_type>
    ruvia::task<void> heartbeat(runtime_type& c, ruvia::db_transaction& transaction,
        const agent_principal& principal, const heartbeat_report& report,
        std::int64_t received_unix_ms, bool commit = false) {
        if (report.node_id_ != principal.node_id_) {
            service::common::throw_app_error(agent_unauthorized);
        }
        const auto runtime_json = heartbeat_runtime_json(c, report);
        const auto update = co_await update_heartbeat(transaction, principal, report, runtime_json, received_unix_ms);
        if (!update) {
            const auto newer = co_await transaction.query(
                "SELECT 1 FROM sys_node WHERE tenant_id=$1 AND id=$2 AND agent_id=$3 "
                "AND deleted_at IS NULL AND registration_status='registered' AND last_heartbeat_at >= to_timestamp($4::double precision/1000.0)",
                principal.tenant_id_, principal.node_id_, principal.agent_id_, received_unix_ms);
            if (!newer.empty()) {
                if (commit) {
                    co_await transaction.commit();
                }
                co_return;
            }
            service::common::throw_app_error(revision_invalid);
        }
        const auto target_changed = co_await service::node_dispatch::mark_node_target_applied(transaction, principal.tenant_id_, report.active_release_id_, report.node_id_);
        const service::node_dispatch::node_apply_attempt confirmed{.tenant_id_ = principal.tenant_id_,
            .release_id_ = report.active_release_id_,
            .node_id_ = report.node_id_,
            .source_ = "heartbeat",
            .applied_ = true,
            .phase_ = {},
            .error_code_ = {},
            .error_ = {},
            .retryable_ = false,
            .elapsed_milliseconds_ = std::nullopt};
        std::vector<service::node_dispatch::recorded_node_attempt> recorded;
        if (target_changed) {
            if (auto attempt = co_await service::node_dispatch::record_node_apply_attempt(transaction, confirmed,
                    c.pool())) {
                recorded.push_back(std::move(*attempt));
            }
        }
        // A heartbeat confirming the applied node spec completes configuration
        // subtasks whose result report was lost.
        for (auto& attempt : co_await service::node_dispatch::record_node_config_attempts(transaction, confirmed,
                 report.applied_node_spec_revision_, c.pool())) {
            recorded.push_back(std::move(attempt));
        }
        // One notification per heartbeat: the online window, the runtime and
        // origin patches, and the transitions this report caused.
        using service::live_resource::resource;
        service::live_resource::notifications::resource_notification notification(principal.tenant_id_);
        notification.node_runtime(principal.node_id_, service::live_resource::runtime_patch(c, report, *update));
        notification.node_seen_unix_ms_ = received_unix_ms;
        service::live_resource::add_origin_runtime(c, notification, report, *update);
        if (update->became_online_) {
            notification.changed(resource::nodes, principal.node_id_)
                .changed(resource::overview)
                .changed(resource::clusters, principal.cluster_id_);
        }
        if (target_changed) {
            add_apply_change(notification, principal);
        }
        add_recorded_attempts(notification, recorded);
        if (commit) {
            co_await service::live_resource::commit_and_notify(transaction, c, notification);
        } else {
            (void)co_await service::live_resource::enqueue_notification(transaction, notification);
        }
        co_return;
    }

    // `elapsed_milliseconds`: time since this control connection sent the
    // desired state the result answers, when known. `trace` is the attempt
    // trace of this apply; it is stored after the result commits.
    template <typename runtime_type>
    ruvia::task<void> record_apply_result(runtime_type& c, const agent_principal& principal,
        const flexedge::node::v2::ApplyResult& result,
        std::optional<std::int64_t> elapsed_milliseconds, service::attempt_trace::recorder& trace) {
        auto transaction = co_await c.db().begin_transaction();
        co_await record_apply_result(c, transaction, principal, result, elapsed_milliseconds, trace.finish(), true);
    }

    template <typename runtime_type>
    ruvia::task<void> record_apply_result(runtime_type& c, ruvia::db_transaction& transaction,
        const agent_principal& principal, const flexedge::node::v2::ApplyResult& result,
        std::optional<std::int64_t> elapsed_milliseconds,
        const std::optional<service::attempt_trace::stored_trace>& trace, bool commit = false) {
        const auto& node_id = principal.node_id_;
        std::vector<service::node_dispatch::recorded_node_attempt> recorded;
        if (result.applied()) {
            const service::node_dispatch::node_apply_attempt applied{.tenant_id_ = principal.tenant_id_,
                .release_id_ = result.release_id(),
                .node_id_ = node_id,
                .source_ = "apply_result",
                .applied_ = true,
                .phase_ = {},
                .error_code_ = {},
                .error_ = {},
                .retryable_ = false,
                .elapsed_milliseconds_ = elapsed_milliseconds};
            if (!(co_await record_applied_deployment(transaction, principal, result.node_spec_revision(),
                    result.release_id(), result.manifest_digest()))) {
                service::common::throw_app_error(revision_invalid);
            }
            const bool target_changed = co_await service::node_dispatch::mark_node_target_applied(
                transaction, principal.tenant_id_, result.release_id(), node_id);
            if (target_changed) {
                if (auto attempt = co_await service::node_dispatch::record_node_apply_attempt(transaction, applied,
                        c.pool())) {
                    recorded.push_back(std::move(*attempt));
                }
            }
            for (auto& attempt : co_await service::node_dispatch::record_node_config_attempts(transaction,
                     applied, result.node_spec_revision(), c.pool())) {
                recorded.push_back(std::move(attempt));
            }
            const auto notification = apply_notification(principal, target_changed, recorded);
            co_await finish_apply(c, transaction, principal, recorded, trace, notification, commit);
            co_return;
        }
        const auto phase = flexedge::node::v2::ApplyPhase_Name(result.failed_phase());
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
        const service::node_dispatch::node_apply_attempt failed{.tenant_id_ = principal.tenant_id_,
            .release_id_ = failure.release_id_,
            .node_id_ = node_id,
            .source_ = "apply_result",
            .applied_ = false,
            .phase_ = apply_phase_name(result.failed_phase()),
            .error_code_ = failure.error_code_,
            .error_ = failure.error_,
            .retryable_ = failure.retryable_,
            .elapsed_milliseconds_ = elapsed_milliseconds};
        bool target_changed = false;
        if (outcome == apply_failure_outcome::recorded) {
            target_changed = co_await service::node_dispatch::mark_node_target_failed(transaction,
                principal.tenant_id_, failure.release_id_, node_id, principal.cluster_id_,
                {.phase_ = failure.phase_, .error_code_ = failure.error_code_, .error_ = failure.error_, .retryable_ = failure.retryable_});
            if (target_changed) {
                if (auto attempt = co_await service::node_dispatch::record_node_apply_attempt(transaction, failed,
                        c.pool())) {
                    recorded.push_back(std::move(*attempt));
                }
            }
            for (auto& attempt : co_await service::node_dispatch::record_node_config_attempts(transaction, failed,
                     failure.revision_, c.pool())) {
                recorded.push_back(std::move(attempt));
            }
        }
        const auto notification = apply_notification(principal, target_changed, recorded);
        co_await finish_apply(c, transaction, principal, recorded, trace, notification, commit);
        co_return;
    }

private:
    static bool hash_matches(std::string_view actual, std::string_view expected) {
        return actual.size() == expected.size() && CRYPTO_memcmp(actual.data(), expected.data(), actual.size()) == 0;
    }

    // Immutable traces and their attempts commit together. A retried report
    // cannot commit the attempt and lose the only copy of its trace.
    template <typename runtime_type>
    static ruvia::task<void> finish_apply(runtime_type& c, ruvia::db_transaction& transaction,
        const agent_principal& principal,
        const std::vector<service::node_dispatch::recorded_node_attempt>& recorded,
        const std::optional<service::attempt_trace::stored_trace>& trace,
        const service::live_resource::notifications::resource_notification& notification, bool commit) {
        if (trace && !recorded.empty()) {
            const auto& first = recorded.front();
            co_await service::attempt_trace::insert_attempt_trace(transaction,
                {.tenant_id_ = principal.tenant_id_, .task_id_ = first.task_id_, .resource_id_ = principal.node_id_, .version_ = first.version_},
                first.event_id_, *trace);
        }
        const auto id = co_await service::live_resource::enqueue_notification(transaction, notification);
        if (commit) {
            co_await transaction.commit();
            co_await service::live_resource::wake_committed_notification(c, id, notification);
        }
    }

    // Task history wording of the node's failure phase.
    static std::string_view apply_phase_name(flexedge::node::v2::ApplyPhase phase) noexcept {
        switch (phase) {
            case flexedge::node::v2::APPLY_PHASE_STAGE:
                return "stage";
            case flexedge::node::v2::APPLY_PHASE_VALIDATE:
                return "validate";
            case flexedge::node::v2::APPLY_PHASE_ACTIVATE:
                return "activate";
            default:
                return "";
        }
    }

    // What an apply result or an applied heartbeat changed: the node, its
    // cluster and websites (deployment state).
    static void add_apply_change(service::live_resource::notifications::resource_notification& notification,
        const agent_principal& principal) {
        using service::live_resource::resource;
        notification.changed(resource::nodes, principal.node_id_)
            .changed(resource::clusters, principal.cluster_id_)
            .changed(resource::websites);
    }

    // The recorded attempts, each for its subtask's task.
    static void add_recorded_attempts(service::live_resource::notifications::resource_notification& notification,
        const std::vector<service::node_dispatch::recorded_node_attempt>& recorded) {
        for (const auto& attempt : recorded) {
            if (!attempt.owner_.empty()) {
                notification.task_attempt(attempt.owner_.operation_id_,
                    {.subtask_id_ = attempt.owner_.subtask_id_, .event_id_ = attempt.event_id_});
            }
        }
    }

    static service::live_resource::notifications::resource_notification apply_notification(const agent_principal& principal,
        bool target_changed, const std::vector<service::node_dispatch::recorded_node_attempt>& recorded) {
        service::live_resource::notifications::resource_notification notification(principal.tenant_id_);
        if (target_changed || !recorded.empty()) {
            add_apply_change(notification, principal);
        }
        add_recorded_attempts(notification, recorded);
        return notification;
    }
};

inline agent_command_service& get_agent_command_service() {
    static agent_command_service service;
    return service;
}

}  // namespace service::agent
