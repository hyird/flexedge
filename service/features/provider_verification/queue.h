#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/db/db_transaction.h>

#include <cstdint>
#include <string_view>

#include "service/features/background/queued_work.h"
#include "service/features/sync_runtime/state.h"

namespace service::provider_verification {

// Verification is a current-state marker. The provider row contains the
// configuration to verify; the marker only wakes the verifier for its latest
// generation and coalesces repeated requests for the same provider.
inline ruvia::task<void> mark_current(ruvia::db_transaction& transaction, std::string_view tenant_id,
    std::string_view provider_id, service::background::queued_work& work) {
    const auto rows = co_await transaction.query(
        "SELECT kind, verification_generation FROM sys_provider WHERE tenant_id = $1 AND id = $2 "
        "AND deleted_at IS NULL LIMIT 1",
        tenant_id, provider_id);
    if (rows.empty()) {
        co_return;
    }
    co_await service::sync_runtime::upsert_marker(
        transaction, tenant_id, service::sync_runtime::marker_resource_type::provider, provider_id,
        service::sync_runtime::marker_operation::verify,
        rows.front()[1].as<std::int64_t>().value_or(1), work);
    co_return;
}

inline ruvia::task<void> remove(ruvia::db_transaction& transaction, std::string_view tenant_id,
    std::string_view provider_id, service::background::queued_work& work) {
    co_await service::sync_runtime::remove_marker(
        transaction, tenant_id, service::sync_runtime::marker_resource_type::provider, provider_id, work);
    co_return;
}

namespace detail {

inline ruvia::task<bool> enqueue(ruvia::db_transaction& transaction, std::string_view tenant_id,
    std::string_view provider_id, std::string_view kind, std::int64_t expected_revision,
    service::background::queued_work& work) {
    const auto rows = co_await transaction.query(
        "UPDATE sys_provider SET verification_generation = verification_generation + 1, "
        "last_error = NULL, status = 'unverified', runtime = '{}'::jsonb, updated_at = NOW() "
        "WHERE id = $1 AND tenant_id = $2 AND kind = $4 AND revision = $3 AND deleted_at IS "
        "NULL RETURNING verification_generation",
        provider_id, tenant_id, expected_revision, kind);
    if (rows.empty()) {
        co_return false;
    }
    co_await service::sync_runtime::upsert_marker(
        transaction, tenant_id, service::sync_runtime::marker_resource_type::provider, provider_id,
        service::sync_runtime::marker_operation::verify,
        rows.front()[0].as<std::int64_t>().value_or(1), work);
    co_return true;
}

}  // namespace detail

inline ruvia::task<bool> enqueue_dns(ruvia::db_transaction& transaction, std::string_view tenant_id,
    std::string_view provider_id, std::int64_t expected_revision, service::background::queued_work& work) {
    co_return co_await detail::enqueue(transaction, tenant_id, provider_id, "dns", expected_revision, work);
}

inline ruvia::task<bool> enqueue_certificate(ruvia::db_transaction& transaction,
    std::string_view tenant_id, std::string_view provider_id,
    std::int64_t expected_revision, service::background::queued_work& work) {
    co_return co_await detail::enqueue(transaction, tenant_id, provider_id, "certificate", expected_revision,
        work);
}

}  // namespace service::provider_verification
