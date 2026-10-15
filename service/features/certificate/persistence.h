#pragma once

#include <ruvia/core/task.h>

#include <stdexcept>
#include <string>
#include <string_view>

#include "service/features/background/worker_pool.h"
#include "service/features/certificate/acme_types.h"
#include "service/features/certificate/attempt_detail.h"
#include "service/features/certificate/task.h"
#include "service/features/certificate_material/model.h"
#include "service/features/node_dispatch/queue.h"
#include "service/features/sync_runtime/error.h"
#include "service/features/sync_runtime/state.h"
#include "service/utils/secret.h"

namespace service::certificate_issuance::worker_detail {

inline ruvia::task<bool>
mark_issuance_started(service::background::worker_context& context, const certificate_task& task,
    const service::sync_runtime::running_marker_lease& lease) {
    auto transaction = co_await context.db().begin_transaction();
    const auto updated = co_await transaction.execute(
        "UPDATE sys_certificate SET status = CASE WHEN issued_revision = 0 THEN 'issuing' ELSE "
        "'renewing' END, last_error = NULL, updated_at = NOW() WHERE id = $1 AND tenant_id = "
        "$2 AND issuance_revision = $3 AND deleted_at IS NULL",
        task.certificate_id_, task.tenant_id_, task.version_);
    if (updated.affected_rows() == 0) {
        (void)co_await service::sync_runtime::release_running(transaction, lease);
        co_await transaction.commit();
        co_return false;
    }
    if (!co_await service::sync_runtime::renew_running_lease(transaction, lease)) {
        throw std::runtime_error("证书同步标记 lease 已失效");
    }
    co_await transaction.commit();
    co_return true;
}

inline ruvia::task<void>
persist_issued_certificate(service::background::worker_context& context, service::attempt_trace::recorder& trace,
    const certificate_task& task,
    const service::sync_runtime::running_marker_lease& lease,
    const issued_certificate& issued, const certificate_attempt& attempt) {
    // Recorded only if this transaction commits; a failure keeps `attempt` at persist.
    auto completed = attempt;
    completed.stage_ = certificate_stage::completed;
    completed.issued_ = issued_certificate_summary{.issuer_ = issued.issuer_,
        .not_before_ = issued.not_before_,
        .not_after_ = issued.expires_at_,
        .serial_number_ = issued.serial_number_,
        .fingerprint_sha256_ = issued.fingerprint_sha256_};
    const auto detail = completed.to_json(context.pool());
    const auto now_rows =
        co_await context.db().query("SELECT TO_CHAR(NOW(), 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF')");
    service::certificate_material::certificate_material_output material(
        {.resource_ = context.pool()});
    material.set<"certificate_chain_pem">(issued.certificate_chain_pem_);
    material.set<"private_key_envelope">(service::utils::seal_secret(issued.private_key_pem_.view()));
    material.set<"not_before">(issued.not_before_);
    material.set<"serial_number">(issued.serial_number_);
    material.set<"fingerprint_sha256">(issued.fingerprint_sha256_);
    material.set<"last_issued_at">(now_rows.front()[0].value().value_or(""));
    const auto material_json = ruvia::to_json(material, {.resource_ = context.pool()});

    auto transaction = co_await context.db().begin_transaction();
    const auto updated = co_await transaction.execute(
        "UPDATE sys_certificate SET material = $1::jsonb, expires_at = $2::timestamptz, "
        "issued_revision = $3, status = 'valid', last_error = NULL, updated_at = NOW() WHERE id = "
        "$4 AND tenant_id = $5 AND issuance_revision = $3 AND deleted_at IS NULL",
        std::string_view(material_json), std::string_view(issued.expires_at_), task.version_,
        task.certificate_id_, task.tenant_id_);
    service::sync_runtime::running_result_transition result_transition;
    // The consumer releases are children of the issuance subtask.
    auto origin = service::operation::work_origin::caused_by(task.owner_);
    if (updated.affected_rows() != 0) {
        if (!co_await service::sync_runtime::renew_running_lease(transaction, lease)) {
            throw std::runtime_error("证书同步标记 lease 已失效");
        }
        co_await service::node_dispatch::enqueue_certificate_consumers(transaction, task.tenant_id_,
            task.certificate_id_, origin);
        result_transition = co_await service::sync_runtime::complete_running_and_record_event(
            transaction, lease, detail);
        if (!result_transition.marker_transitioned_) {
            throw std::runtime_error("证书同步标记 lease 已失效");
        }
    } else {
        (void)co_await service::sync_runtime::release_running(transaction, lease);
    }
    co_await service::sync_runtime::commit_and_publish_result_event(transaction, context, lease,
        result_transition, trace, true, &origin);
    co_return;
}

inline ruvia::task<void> fail_certificate_task(service::background::worker_context& context,
    service::attempt_trace::recorder& trace, const certificate_task& task, std::string_view error,
    bool permanent, std::string_view detail) {
    const auto message = service::sync_runtime::attempt_error(trace, error);
    const auto lease = service::sync_runtime::make_running_lease(
        task.tenant_id_, task.id_, task.version_, context.lease_owner(), task.claimed_at_,
        service::sync_runtime::marker_resource_type::certificate, task.certificate_id_);
    auto transaction = co_await context.db().begin_transaction();
    (void)co_await transaction.query(
        "SELECT id FROM sys_certificate WHERE tenant_id = $1 AND id = $2 LIMIT 1 FOR UPDATE",
        task.tenant_id_, task.certificate_id_);
    const auto result_transition = co_await service::sync_runtime::fail_running_and_record_event(
        transaction, lease, message, detail);
    // Expired material leaves the consumers: their releases belong to the
    // subtask the failed attempt was recorded for.
    auto origin = service::operation::work_origin::caused_by(result_transition.owner_);
    if (result_transition.marker_transitioned_) {
        const auto updated = co_await transaction.query(
            "UPDATE sys_certificate SET status = CASE WHEN expires_at > NOW() THEN CASE WHEN $3 "
            "THEN 'valid' ELSE 'renewing' END WHEN $3 THEN 'failed' ELSE 'pending' END, "
            "last_error = $4, updated_at = NOW() WHERE id = $1 AND tenant_id = $5 AND "
            "issuance_revision = $2 AND deleted_at IS NULL RETURNING issued_revision > 0 AND "
            "expires_at <= NOW()",
            task.certificate_id_, task.version_, permanent, std::string_view(message), task.tenant_id_);
        if (!updated.empty() && updated.front()[0].as<bool>().value_or(false)) {
            co_await service::node_dispatch::enqueue_certificate_consumers(transaction, task.tenant_id_,
                task.certificate_id_, origin);
        }
    }
    co_await service::sync_runtime::commit_and_publish_result_event(transaction, context, lease,
        result_transition, trace, true, &origin);
    if (!result_transition.marker_transitioned_) {
        co_return;
    }
    service::sync_runtime::log_attempt_failure("Certificate sync marker", task.id_, message);
    co_return;
}

}  // namespace service::certificate_issuance::worker_detail
