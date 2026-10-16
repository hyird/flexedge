#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/db/db_transaction.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <openssl/crypto.h>

#include "node/proto/edge_control.pb.h"
#include "service/common/http.h"
#include "service/features/attempt_trace/recorder.h"
#include "service/features/attempt_trace/trace_store.h"
#include "service/features/background/queued_work.h"
#include "service/features/dns_sync/queue.h"
#include "service/features/live_resource/notification_outbox.h"
#include "service/features/node_channel/apply_result_store.h"
#include "service/features/node_channel/error.h"
#include "service/features/node_channel/heartbeat_store.h"
#include "service/features/node_channel/runtime_mapper.h"
#include "service/features/node_channel/types.h"
#include "service/features/node_dispatch/apply_history.h"
#include "service/features/node_dispatch/queue.h"
#include "service/features/node_dispatch/target_store.h"
#include "service/features/sync_runtime/work_commit.h"

// The durable effects of the node reports a claiming Worker persists. Each
// runs inside the claim transaction, which commits the report receipt with
// them; work and notifications they queue are claimed at the next scan.
namespace service::node_channel {

namespace report_detail {

inline bool hash_matches(std::string_view actual, std::string_view expected) {
    return actual.size() == expected.size() && CRYPTO_memcmp(actual.data(), expected.data(), actual.size()) == 0;
}

// Task history wording of the node's failure phase.
inline std::string_view apply_phase_name(flexedge::node::v2::ApplyPhase phase) noexcept {
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
inline void add_apply_change(service::live_resource::notifications::resource_notification& notification,
    const agent_principal& principal) {
    using service::live_resource::resource;
    notification.changed(resource::nodes, principal.node_id_)
        .changed(resource::clusters, principal.cluster_id_)
        .changed(resource::websites);
}

// The node task (task id = node id): its recorded attempts, and a change
// whenever the report changed a target, the applied revision or recorded
// an attempt.
inline void add_node_task_change(service::live_resource::notifications::resource_notification& notification,
    const agent_principal& principal, bool deployment_changed,
    const std::vector<service::node_dispatch::recorded_node_attempt>& recorded) {
    for (const auto& attempt : recorded) {
        notification.task_attempt(principal.node_id_, attempt.event_id_);
    }
    if (deployment_changed || !recorded.empty()) {
        notification.changed(service::live_resource::resource::tasks, principal.node_id_);
    }
}

// Immutable traces and their attempts commit together. A retried report
// cannot commit the attempt and lose the only copy of its trace.
inline ruvia::task<void> finish_apply(ruvia::db_transaction& transaction, const agent_principal& principal,
    bool deployment_changed, const std::vector<service::node_dispatch::recorded_node_attempt>& recorded,
    const std::optional<service::attempt_trace::stored_trace>& trace) {
    if (trace && !recorded.empty()) {
        const auto& first = recorded.front();
        co_await service::attempt_trace::insert_attempt_trace(transaction,
            {.tenant_id_ = principal.tenant_id_, .task_id_ = first.task_id_, .resource_id_ = principal.node_id_, .version_ = first.version_},
            first.event_id_, *trace);
    }
    service::live_resource::notifications::resource_notification notification(principal.tenant_id_);
    if (deployment_changed || !recorded.empty()) {
        add_apply_change(notification, principal);
    }
    add_node_task_change(notification, principal, deployment_changed, recorded);
    (void)co_await service::live_resource::enqueue_notification(transaction, notification);
}

}  // namespace report_detail

// The authenticate report: verifies the credentials it was accepted with
// and registers a pending node.
inline ruvia::task<agent_principal> record_authentication(ruvia::db_transaction& transaction,
    const std::string& agent_id, std::string_view verified_hash, std::int64_t received_unix_ms) {
    const auto candidates = co_await transaction.query(
        "SELECT id, cluster_id, tenant_id, registration_status, node_secret_hash FROM "
        "sys_node WHERE agent_id = $1 AND deleted_at IS NULL LIMIT 1",
        agent_id);
    if (candidates.empty()) {
        service::common::throw_app_error(agent_unauthorized);
    }
    const auto stored_hash = candidates.front()[4].value();
    if (!stored_hash || !report_detail::hash_matches(verified_hash, *stored_hash)) {
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
    if (!current_hash || !report_detail::hash_matches(verified_hash, *current_hash)) {
        service::common::throw_app_error(agent_unauthorized);
    }
    // Work an agent's authentication causes: the first registration of a
    // node publishes a release including it, and a reconnect publishes a
    // release of the current schema when the cluster has none.
    service::background::queued_work work;
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
            cluster_id, work);
        co_await service::dns_sync::mark_cluster_zone_dirty(transaction, tenant_id, cluster_id, work);
    } else if (rows.front()[3].value().value_or("") == "registered") {
        co_await service::node_dispatch::ensure_cluster_release(transaction, tenant_id, cluster_id, work);
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
    notification.node_seen_unix_ms_ = received_unix_ms;
    service::sync_runtime::add_changed_tasks(notification, work);
    (void)co_await service::live_resource::enqueue_notification(transaction, notification);
    co_return result;
}

template <typename context_type>
ruvia::task<void> record_heartbeat(context_type& context, ruvia::db_transaction& transaction,
    const agent_principal& principal, const heartbeat_report& report, std::int64_t received_unix_ms) {
    if (report.node_id_ != principal.node_id_) {
        service::common::throw_app_error(agent_unauthorized);
    }
    const auto runtime_json = stored_runtime_json(context, report);
    const auto previous_revision = co_await lock_applied_node_spec_revision(transaction, principal);
    const auto update = co_await update_heartbeat(transaction, principal, report, runtime_json, received_unix_ms);
    if (!update) {
        const auto newer = co_await transaction.query(
            "SELECT 1 FROM sys_node WHERE tenant_id=$1 AND id=$2 AND agent_id=$3 "
            "AND deleted_at IS NULL AND registration_status='registered' AND last_heartbeat_at >= to_timestamp($4::double precision/1000.0)",
            principal.tenant_id_, principal.node_id_, principal.agent_id_, received_unix_ms);
        if (!newer.empty()) {
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
                context.pool())) {
            recorded.push_back(std::move(*attempt));
        }
    }
    // A heartbeat raising the applied node spec revision records the
    // configuration attempt whose result report was lost.
    const bool revision_raised = previous_revision && *previous_revision < report.applied_node_spec_revision_;
    if (revision_raised) {
        if (auto attempt = co_await service::node_dispatch::record_node_config_attempt(transaction, confirmed,
                report.applied_node_spec_revision_, context.pool())) {
            recorded.push_back(std::move(*attempt));
        }
    }
    // One notification per heartbeat: the online window, the runtime and
    // origin patches, and the transitions this report caused.
    using service::live_resource::resource;
    service::live_resource::notifications::resource_notification notification(principal.tenant_id_);
    notification.node_runtime(principal.node_id_, runtime_patch(context, report, *update));
    notification.node_seen_unix_ms_ = received_unix_ms;
    add_origin_runtime(context, notification, report, *update);
    if (update->became_online_) {
        notification.changed(resource::nodes, principal.node_id_)
            .changed(resource::overview)
            .changed(resource::clusters, principal.cluster_id_);
    }
    if (target_changed) {
        report_detail::add_apply_change(notification, principal);
    }
    report_detail::add_node_task_change(notification, principal, target_changed || revision_raised, recorded);
    (void)co_await service::live_resource::enqueue_notification(transaction, notification);
}

// `elapsed_milliseconds`: time since the control connection sent the desired
// state the result answers, when known. `trace` is the attempt trace of this
// apply.
template <typename context_type>
ruvia::task<void> record_apply_result(context_type& context, ruvia::db_transaction& transaction,
    const agent_principal& principal, const flexedge::node::v2::ApplyResult& result,
    std::optional<std::int64_t> elapsed_milliseconds,
    const std::optional<service::attempt_trace::stored_trace>& trace) {
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
        const auto previous_revision = co_await lock_applied_node_spec_revision(transaction, principal);
        if (!previous_revision ||
            !(co_await record_applied_deployment(transaction, principal, result.node_spec_revision(),
                result.release_id(), result.manifest_digest()))) {
            service::common::throw_app_error(revision_invalid);
        }
        const bool target_changed = co_await service::node_dispatch::mark_node_target_applied(
            transaction, principal.tenant_id_, result.release_id(), node_id);
        if (target_changed) {
            if (auto attempt = co_await service::node_dispatch::record_node_apply_attempt(transaction, applied,
                    context.pool())) {
                recorded.push_back(std::move(*attempt));
            }
        }
        const bool revision_raised = *previous_revision < result.node_spec_revision();
        if (revision_raised) {
            if (auto attempt = co_await service::node_dispatch::record_node_config_attempt(transaction, applied,
                    result.node_spec_revision(), context.pool())) {
                recorded.push_back(std::move(*attempt));
            }
        }
        co_await report_detail::finish_apply(transaction, principal, target_changed || revision_raised, recorded, trace);
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
    const auto previous_revision = co_await lock_applied_node_spec_revision(transaction, principal);
    if (!previous_revision) {
        service::common::throw_app_error(revision_invalid);
    }
    const auto outcome = co_await record_failed_deployment(transaction, principal, failure);
    if (outcome == apply_failure_outcome::rejected) {
        service::common::throw_app_error(revision_invalid);
    }
    const service::node_dispatch::node_apply_attempt failed{.tenant_id_ = principal.tenant_id_,
        .release_id_ = failure.release_id_,
        .node_id_ = node_id,
        .source_ = "apply_result",
        .applied_ = false,
        .phase_ = report_detail::apply_phase_name(result.failed_phase()),
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
                    context.pool())) {
                recorded.push_back(std::move(*attempt));
            }
        }
        if (failure.revision_ > *previous_revision) {
            if (auto attempt = co_await service::node_dispatch::record_node_config_attempt(transaction, failed,
                    failure.revision_, context.pool())) {
                recorded.push_back(std::move(*attempt));
            }
        }
    }
    co_await report_detail::finish_apply(transaction, principal, target_changed, recorded, trace);
}

}  // namespace service::node_channel
