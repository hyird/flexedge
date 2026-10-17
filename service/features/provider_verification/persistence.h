#pragma once

#include <ruvia/core/task.h>

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "service/features/background/queued_work.h"
#include "service/features/background/task_context.h"
#include "service/features/certificate/provider_runtime_credentials.h"
#include "service/features/dns_sync/queue.h"
#include "service/features/outbound_http/error.h"
#include "service/features/provider_verification/queue.h"
#include "service/features/provider_verification/task.h"
#include "service/features/provider_verification/verification.h"
#include "service/features/sync_runtime/error.h"
#include "service/features/sync_runtime/state.h"
#include "service/features/sync_runtime/work_commit.h"

namespace service::provider_verification::detail {

inline ruvia::task<void> reconcile(service::background::task_context& context) {
    ruvia::db_query candidates(context.pool());
    candidates.select({candidates.column("tenant_id", "provider"), candidates.column("id", "provider"),
                          candidates.column("verification_generation", "provider")})
        .from("sys_provider", "provider")
        .where(candidates.sql(
            "provider.deleted_at IS NULL AND NOT EXISTS (SELECT 1 FROM sys_sync_task marker "
            "WHERE marker.resource_type = 'provider' AND marker.tenant_id = provider.tenant_id "
            "AND marker.resource_id = provider.id AND marker.version = provider.verification_generation)"))
        .limit(64);
    const auto missing = co_await context.db().query(candidates);
    for (const auto& row : missing) {
        const auto tenant_id = std::string(row[0].value().value_or(""));
        const auto provider_id = std::string(row[1].value().value_or(""));
        const auto version = row[2].as<std::int64_t>().value_or(1);
        auto transaction = co_await context.db().begin_transaction();
        ruvia::db_query aggregate(context.pool());
        aggregate.select(aggregate.column("id"))
            .from("sys_provider")
            .where(aggregate.sql({"tenant_id = ", " AND id = ", " AND verification_generation = ",
                                     " AND deleted_at IS NULL"},
                {aggregate.value(tenant_id), aggregate.value(provider_id), aggregate.value(version)}))
            .limit(1)
            .lock({.mode_ = ruvia::db_row_lock::update});
        const auto locked = co_await transaction.query(aggregate);
        if (locked.empty() || co_await service::sync_runtime::marker_version_exists(transaction, tenant_id,
                                  service::sync_runtime::marker_resource_type::provider, provider_id, version)) {
            co_await transaction.commit();
            continue;
        }
        service::background::queued_work work;
        (void)co_await service::sync_runtime::upsert_marker(transaction, tenant_id,
            service::sync_runtime::marker_resource_type::provider, provider_id,
            service::sync_runtime::marker_operation::verify, version, work);
        co_await service::sync_runtime::commit_work(transaction, context, tenant_id, work);
    }
}

inline ruvia::task<std::optional<std::string>>
load_current_runtime(service::background::task_context& context, const verification_task& task) {
    const auto lease = service::sync_runtime::make_running_lease(
        task.tenant_id_, task.id_, task.generation_, context.lease_owner(), task.claimed_at_,
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

inline ruvia::task<void> complete_verification(service::background::task_context& context,
    service::attempt_trace::recorder& trace, const verification_task& task,
    verification_result result, std::string_view detail) {
    const bool dns = task.kind_ == "dns";
    const auto lease = service::sync_runtime::make_running_lease(
        task.tenant_id_, task.id_, task.generation_, context.lease_owner(), task.claimed_at_,
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
            throw service::outbound_http::attempt_error("DNS 服务商检测结果缺失", true);
        }
        runtime_json = std::move(*result.dns_runtime_);
    } else {
        const auto runtime = service::certificate_issuance::parse_certificate_provider_runtime(
            rows.front()[0].value().value_or("{}"), {.resource_ = context.pool()});
        if (!runtime) {
            throw service::outbound_http::attempt_error("证书供应商 runtime 损坏", true);
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
    // A verified DNS provider re-syncs its zones.
    service::background::queued_work work;
    if (updated.affected_rows() == 0) {
        (void)co_await service::sync_runtime::release_running(transaction, lease);
    } else {
        if (dns) {
            co_await service::dns_sync::mark_provider_zones_dirty(transaction, task.tenant_id_,
                task.provider_id_, work);
        }
        result_transition = co_await service::sync_runtime::record_running_result(transaction, lease, service::sync_runtime::attempt_result::completed(), detail);
        if (!result_transition.marker_transitioned_) {
            throw std::runtime_error("供应商检测标记 lease 已失效");
        }
    }
    co_await service::sync_runtime::commit_and_publish_result_event(transaction, context, lease,
        result_transition, trace, false, &work);
    co_return;
}

inline ruvia::task<void> fail_verification(service::background::task_context& context,
    service::attempt_trace::recorder& trace, const verification_task& task, std::string_view error,
    bool permanent, std::string_view detail) {
    const auto message = service::sync_runtime::attempt_error(trace, error);
    const auto lease = service::sync_runtime::make_running_lease(
        task.tenant_id_, task.id_, task.generation_, context.lease_owner(), task.claimed_at_,
        service::sync_runtime::marker_resource_type::provider, task.provider_id_);
    auto transaction = co_await context.db().begin_transaction();
    const auto result_transition = co_await service::sync_runtime::record_running_result(
        transaction, lease, permanent ? service::sync_runtime::attempt_result::failed(message) : service::sync_runtime::attempt_result::retry(message), detail);
    if (result_transition.marker_transitioned_) {
        (void)co_await transaction.execute(
            "UPDATE sys_provider SET status = CASE WHEN $1::BOOLEAN THEN 'invalid' ELSE status "
            "END, last_error = $2, updated_at = NOW() WHERE id = $3 AND tenant_id = $4 AND "
            "verification_generation = $5 AND deleted_at IS NULL",
            permanent, std::string_view(message), task.provider_id_, task.tenant_id_, task.generation_);
    }
    co_await service::sync_runtime::commit_and_publish_result_event(transaction, context, lease,
        result_transition, trace);
    if (result_transition.marker_transitioned_) {
        service::sync_runtime::log_attempt_failure("Provider verification marker", task.id_, message);
    }
    co_return;
}

}  // namespace service::provider_verification::detail
