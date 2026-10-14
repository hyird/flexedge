#pragma once

#include <ruvia/core/task.h>

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "service/features/background/worker_pool.h"
#include "service/features/certificate/provider_runtime_credentials.h"
#include "service/features/dns_sync/queue.h"
#include "service/features/logging/logger.h"
#include "service/features/provider_verification/failure.h"
#include "service/features/provider_verification/queue.h"
#include "service/features/provider_verification/task.h"
#include "service/features/provider_verification/verification.h"
#include "service/features/sync_runtime/error.h"
#include "service/features/sync_runtime/state.h"

namespace service::provider_verification::detail {

inline ruvia::task<void> reconcile(service::background::worker_context& context) {
    const auto rows = co_await context.db().query(
        "SELECT marker.tenant_id, marker.id, marker.resource_id, provider.verification_generation "
        "FROM sys_sync_task marker INNER JOIN sys_provider provider ON provider.tenant_id = "
        "marker.tenant_id AND provider.id = marker.resource_id WHERE marker.resource_type = "
        "'provider' AND provider.deleted_at IS NULL AND marker.version <> "
        "provider.verification_generation LIMIT 64");
    for (const auto& row : rows) {
        auto transaction = co_await context.db().begin_transaction();
        const auto provider_id = std::string(row[2].value().value_or(""));
        co_await service::provider_verification::mark_current(
            transaction, row[0].value().value_or(""), provider_id);
        co_await transaction.commit();
    }
    co_return;
}

inline ruvia::task<std::optional<std::string>>
load_current_runtime(service::background::worker_context& context, const verification_task& task) {
    const auto lease = service::sync_runtime::make_running_lease(
        task.tenant_id_, task.id_, task.generation_, context.lease_owner(),
        service::sync_runtime::marker_resource_type::provider, task.provider_id_);
    auto db = context.db();
    if (!co_await service::sync_runtime::renew_running_lease(db, lease)) {
        throw std::runtime_error("供应商检测标记 lease 已失效");
    }
    const auto rows = co_await db.query(
        "SELECT runtime::text FROM sys_provider WHERE id = $1 AND tenant_id = $2 AND kind = $3 "
        "AND revision = $4 AND verification_generation = $5 AND deleted_at IS NULL LIMIT 1",
        task.provider_id_, task.tenant_id_, task.kind_, task.provider_revision_, task.generation_);
    if (!rows.empty()) {
        co_return std::string(rows.front()[0].value().value_or("{}"));
    }
    auto transaction = co_await db.begin_transaction();
    (void)co_await service::sync_runtime::release_running(transaction, lease);
    co_await transaction.commit();
    co_return std::nullopt;
}

inline ruvia::task<void> complete_verification(service::background::worker_context& context,
    const verification_task& task,
    verification_result result) {
    const bool dns = task.kind_ == "dns";
    const auto lease = service::sync_runtime::make_running_lease(
        task.tenant_id_, task.id_, task.generation_, context.lease_owner(),
        service::sync_runtime::marker_resource_type::provider, task.provider_id_);
    auto transaction = co_await context.db().begin_transaction();
    const auto rows = co_await transaction.query(
        "SELECT runtime::text FROM sys_provider WHERE id = $1 AND tenant_id = $2 AND kind = $3 "
        "AND revision = $4 AND verification_generation = $5 AND deleted_at IS NULL LIMIT 1 FOR "
        "UPDATE",
        task.provider_id_, task.tenant_id_, task.kind_, task.provider_revision_, task.generation_);
    if (rows.empty()) {
        (void)co_await service::sync_runtime::release_running(transaction, lease);
        co_await transaction.commit();
        co_return;
    }
    if (!co_await service::sync_runtime::renew_running_lease(transaction, lease)) {
        throw std::runtime_error("供应商检测标记 lease 已失效");
    }

    std::string runtime_json;
    if (dns) {
        if (!result.dns_runtime_) {
            throw verification_error("DNS 服务商检测结果缺失", true);
        }
        runtime_json = std::move(*result.dns_runtime_);
    } else {
        const auto runtime = service::certificate_issuance::parse_certificate_provider_runtime(
            rows.front()[0].value().value_or("{}"), {.resource_ = context.pool()});
        if (!runtime) {
            throw verification_error("证书供应商 runtime 损坏", true);
        }
        runtime_json = service::certificate_issuance::serialize_certificate_provider_runtime(
            runtime, result.eab_, context.pool());
    }
    const auto updated = co_await transaction.execute(
        "UPDATE sys_provider SET runtime = $1::jsonb, status = 'verified', last_verified_at = "
        "NOW(), last_error = NULL, updated_at = NOW() WHERE id = $2 AND tenant_id = $3 AND "
        "revision = $4 AND verification_generation = $5 AND deleted_at IS NULL",
        std::string_view(runtime_json), task.provider_id_, task.tenant_id_, task.provider_revision_,
        task.generation_);
    service::sync_runtime::running_result_transition result_transition;
    if (updated.affected_rows() == 0) {
        (void)co_await service::sync_runtime::release_running(transaction, lease);
    } else {
        if (dns) {
            co_await service::dns_sync::mark_provider_zones_dirty(transaction, task.tenant_id_,
                task.provider_id_);
        }
        result_transition =
            co_await service::sync_runtime::complete_running_and_record_event(transaction, lease);
        if (!result_transition.marker_transitioned_) {
            throw std::runtime_error("供应商检测标记 lease 已失效");
        }
    }
    co_await service::sync_runtime::commit_and_publish_result_event(transaction, lease,
        result_transition);
    co_return;
}

inline ruvia::task<void> fail_verification(service::background::worker_context& context,
    const verification_task& task, std::string_view error,
    bool permanent) {
    const auto message = service::sync_runtime::bounded_error(error);
    const auto lease = service::sync_runtime::make_running_lease(
        task.tenant_id_, task.id_, task.generation_, context.lease_owner(),
        service::sync_runtime::marker_resource_type::provider, task.provider_id_);
    auto transaction = co_await context.db().begin_transaction();
    const auto result_transition =
        co_await service::sync_runtime::fail_running_and_record_event(transaction, lease, message);
    if (result_transition.marker_transitioned_) {
        (void)co_await transaction.execute(
            "UPDATE sys_provider SET status = CASE WHEN $1::BOOLEAN THEN 'invalid' ELSE status "
            "END, last_error = $2, updated_at = NOW() WHERE id = $3 AND tenant_id = $4 AND "
            "verification_generation = $5 AND deleted_at IS NULL",
            permanent, std::string_view(message), task.provider_id_, task.tenant_id_, task.generation_);
    }
    co_await service::sync_runtime::commit_and_publish_result_event(transaction, lease,
        result_transition);
    if (result_transition.marker_transitioned_) {
        service::logging::error("Provider verification marker " + task.id_ + " failed: " + message);
    }
    co_return;
}

}  // namespace service::provider_verification::detail
