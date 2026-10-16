#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/db/db_transaction.h>

#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include "service/features/background/queued_work.h"
#include "service/features/background/task_context.h"
#include "service/features/certificate/acme_types.h"
#include "service/features/certificate/attempt_detail.h"
#include "service/features/certificate/issuance_state.h"
#include "service/features/certificate/task.h"
#include "service/features/certificate/work_loader.h"
#include "service/features/certificate_material/model.h"
#include "service/features/dns_sync/queue.h"
#include "service/features/node_dispatch/queue.h"
#include "service/features/sync_runtime/error.h"
#include "service/features/sync_runtime/state.h"
#include "service/features/sync_runtime/work_commit.h"
#include "service/utils/secret.h"

namespace service::certificate_issuance::worker_detail {

inline ruvia::task<bool>
mark_issuance_started(service::background::task_context& context, const certificate_task& task,
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

// Writes `progress` for the task's issuance revision. False when the
// certificate or its revision is gone; the lease is then released.
inline ruvia::task<bool> store_progress(ruvia::db_transaction& transaction, service::background::task_context& context,
    const certificate_task& task, const service::sync_runtime::running_marker_lease& lease,
    const issuance_state& progress) {
    const auto progress_json = serialize_issuance_state(progress, context.pool());
    const auto updated = co_await transaction.execute(
        "UPDATE sys_certificate SET issuance_state = $1::jsonb, updated_at = NOW() WHERE id = $2 AND tenant_id = "
        "$3 AND issuance_revision = $4 AND deleted_at IS NULL",
        std::string_view(progress_json), task.certificate_id_, task.tenant_id_, task.version_);
    if (updated.affected_rows() == 0) {
        (void)co_await service::sync_runtime::release_running(transaction, lease);
        co_return false;
    }
    co_return true;
}

// Publishes the order's DNS-01 TXT records and parks the task until the DNS
// zone's sync task completes the revision that carries them: the attempt is
// recorded as progressed and the DNS result resumes the task at `validation`.
inline ruvia::task<void> publish_challenges(service::background::task_context& context,
    service::attempt_trace::recorder& trace, service::attempt_trace::step_scope& step, const certificate_task& task,
    const service::sync_runtime::running_marker_lease& lease, const certificate_work& work,
    const issuance_state& progress, std::span<const service::dns_sync::challenge_record_spec> records,
    const certificate_attempt& attempt) {
    auto transaction = co_await context.db().begin_transaction();
    if (!co_await store_progress(transaction, context, task, lease, progress)) {
        co_await transaction.commit();
        co_return;
    }
    service::background::queued_work work_queue;
    const auto zone = co_await service::dns_sync::replace_challenge_records(transaction, task.tenant_id_,
        work.dns_zone_id_, task.certificate_id_, records, work.dns_minimum_record_ttl_, work_queue);
    if (!zone) {
        throw acme_error("DNS-01 托管域名不存在", false);
    }
    trace.link_task({.resource_type_ = "dns_zone", .resource_id_ = work.dns_zone_id_, .label_ = "DNS 同步"});
    trace.log(service::attempt_trace::log_level::info,
        "已写入 " + std::to_string(records.size()) + " 条 _acme-challenge TXT 记录，DNS 同步完成后继续验证");
    const auto result_transition = co_await service::sync_runtime::park_running_and_record_event(transaction, lease,
        zone->task_id_, zone->desired_revision_, attempt.to_json(context.pool()));
    if (!result_transition.marker_transitioned_) {
        throw std::runtime_error("证书同步标记 lease 已失效");
    }
    step.complete();
    co_await service::sync_runtime::commit_and_publish_result_event(transaction, context, lease, result_transition,
        trace, false, &work_queue);
}

// The authorizations are valid: retires the challenge records and stores the
// finalize progress (with the sealed certificate key) before the order is
// finalized. False when the issuance revision is gone.
inline ruvia::task<bool> enter_finalize(service::background::task_context& context, const certificate_task& task,
    const service::sync_runtime::running_marker_lease& lease, const certificate_work& work,
    const issuance_state& progress) {
    auto transaction = co_await context.db().begin_transaction();
    if (!co_await store_progress(transaction, context, task, lease, progress)) {
        co_await transaction.commit();
        co_return false;
    }
    service::background::queued_work work_queue;
    (void)co_await service::dns_sync::replace_challenge_records(transaction, task.tenant_id_, work.dns_zone_id_,
        task.certificate_id_, {}, 0, work_queue);
    if (!co_await service::sync_runtime::renew_running_lease(transaction, lease)) {
        throw std::runtime_error("证书同步标记 lease 已失效");
    }
    co_await service::sync_runtime::commit_work(transaction, context, task.tenant_id_, work_queue);
    co_return true;
}

inline ruvia::task<void>
persist_issued_certificate(service::background::task_context& context, service::attempt_trace::recorder& trace,
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
        "issued_revision = $3, status = 'valid', last_error = NULL, issuance_state = NULL, updated_at = NOW() "
        "WHERE id = "
        "$4 AND tenant_id = $5 AND issuance_revision = $3 AND deleted_at IS NULL",
        std::string_view(material_json), std::string_view(issued.expires_at_), task.version_,
        task.certificate_id_, task.tenant_id_);
    service::sync_runtime::running_result_transition result_transition;
    service::background::queued_work work;
    if (updated.affected_rows() != 0) {
        if (!co_await service::sync_runtime::renew_running_lease(transaction, lease)) {
            throw std::runtime_error("证书同步标记 lease 已失效");
        }
        co_await service::node_dispatch::enqueue_certificate_consumers(transaction, task.tenant_id_,
            task.certificate_id_, work);
        result_transition = co_await service::sync_runtime::complete_running_and_record_event(
            transaction, lease, detail);
        if (!result_transition.marker_transitioned_) {
            throw std::runtime_error("证书同步标记 lease 已失效");
        }
    } else {
        (void)co_await service::sync_runtime::release_running(transaction, lease);
    }
    co_await service::sync_runtime::commit_and_publish_result_event(transaction, context, lease,
        result_transition, trace, true, &work);
    co_return;
}

// Records the failed attempt. Unless `keep_progress` (the order is still
// usable: the ACME server was still working or unreachable), the stored
// progress is dropped and its challenge records retired, so the retry starts
// a new order.
inline ruvia::task<void> fail_certificate_task(service::background::task_context& context,
    service::attempt_trace::recorder& trace, const certificate_task& task, std::string_view error,
    bool permanent, bool keep_progress, std::string_view detail) {
    const auto message = service::sync_runtime::attempt_error(trace, error);
    const auto lease = service::sync_runtime::make_running_lease(
        task.tenant_id_, task.id_, task.version_, context.lease_owner(), task.claimed_at_,
        service::sync_runtime::marker_resource_type::certificate, task.certificate_id_);
    auto transaction = co_await context.db().begin_transaction();
    const auto certificate = co_await transaction.query(
        "SELECT dns_zone_id FROM sys_certificate WHERE tenant_id = $1 AND id = $2 LIMIT 1 FOR UPDATE",
        task.tenant_id_, task.certificate_id_);
    const auto result_transition = co_await service::sync_runtime::fail_running_and_record_event(
        transaction, lease, message, detail);
    // Expired material leaves the consumers.
    service::background::queued_work work;
    if (result_transition.marker_transitioned_) {
        const auto updated = co_await transaction.query(
            "UPDATE sys_certificate SET status = CASE WHEN expires_at > NOW() THEN CASE WHEN $3 "
            "THEN 'valid' ELSE 'renewing' END WHEN $3 THEN 'failed' ELSE 'pending' END, "
            "last_error = $4, issuance_state = CASE WHEN $6 THEN issuance_state END, updated_at = NOW() "
            "WHERE id = $1 AND tenant_id = $5 AND issuance_revision = $2 AND deleted_at IS NULL RETURNING "
            "issued_revision > 0 AND expires_at <= NOW()",
            task.certificate_id_, task.version_, permanent, std::string_view(message), task.tenant_id_,
            keep_progress);
        if (!updated.empty() && updated.front()[0].as<bool>().value_or(false)) {
            co_await service::node_dispatch::enqueue_certificate_consumers(transaction, task.tenant_id_,
                task.certificate_id_, work);
        }
        if (!keep_progress && !certificate.empty()) {
            (void)co_await service::dns_sync::replace_challenge_records(transaction, task.tenant_id_,
                certificate.front()[0].value().value_or(""), task.certificate_id_, {}, 0, work);
        }
    }
    co_await service::sync_runtime::commit_and_publish_result_event(transaction, context, lease,
        result_transition, trace, true, &work);
    if (!result_transition.marker_transitioned_) {
        co_return;
    }
    service::sync_runtime::log_attempt_failure("Certificate sync marker", task.id_, message);
    co_return;
}

}  // namespace service::certificate_issuance::worker_detail
