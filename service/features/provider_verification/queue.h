#pragma once

#include <cstdint>
#include <string_view>

#include <ruvia/core/Task.h>
#include <ruvia/web/db/DbTransaction.h>

#include "service/features/sync_runtime/state.h"

namespace service::provider_verification {

// Verification is a current-state marker. The provider row contains the
// configuration to verify; the marker only wakes the verifier for its latest
// generation and coalesces repeated requests for the same provider.
inline ruvia::Task<void> markCurrent(ruvia::DbTransaction& transaction, std::string_view tenantId,
                                     std::string_view providerId) {
    const auto rows = co_await transaction.query(
        "SELECT kind, verification_generation FROM sys_provider WHERE tenant_id = $1 AND id = $2 "
        "AND deleted_at IS NULL LIMIT 1",
        tenantId, providerId);
    if (rows.empty()) {
        co_return;
    }
    co_await service::sync_runtime::upsertMarker(transaction, tenantId, "provider", providerId,
                                                 "verify",
                                                 rows.front()[1].as<std::int64_t>().value_or(1));
    co_return;
}

inline ruvia::Task<void> remove(ruvia::DbTransaction& transaction, std::string_view tenantId,
                                std::string_view providerId) {
    co_await service::sync_runtime::removeMarker(transaction, tenantId, "provider", providerId);
    co_return;
}

inline ruvia::Task<bool> enqueueDns(ruvia::DbTransaction& transaction, std::string_view tenantId,
                                    std::string_view providerId, std::int64_t expectedRevision) {
    const auto rows = co_await transaction.query(
        "UPDATE sys_provider SET verification_generation = verification_generation + 1, "
        "last_error = NULL, status = 'unverified', runtime = '{}'::jsonb, updated_at = NOW() "
        "WHERE id = $1 AND tenant_id = $2 AND kind = 'dns' AND revision = $3 AND deleted_at IS "
        "NULL RETURNING verification_generation",
        providerId, tenantId, expectedRevision);
    if (rows.empty()) {
        co_return false;
    }
    co_await service::sync_runtime::upsertMarker(transaction, tenantId, "provider", providerId,
                                                 "verify",
                                                 rows.front()[0].as<std::int64_t>().value_or(1));
    co_return true;
}

inline ruvia::Task<bool> enqueueCertificate(ruvia::DbTransaction& transaction,
                                            std::string_view tenantId, std::string_view providerId,
                                            std::int64_t expectedRevision) {
    const auto rows = co_await transaction.query(
        "UPDATE sys_provider SET verification_generation = verification_generation + 1, "
        "last_error = NULL, status = 'unverified', runtime = '{}'::jsonb, updated_at = NOW() "
        "WHERE id = $1 AND tenant_id = $2 AND kind = 'certificate' AND revision = $3 AND "
        "deleted_at IS NULL RETURNING verification_generation",
        providerId, tenantId, expectedRevision);
    if (rows.empty()) {
        co_return false;
    }
    co_await service::sync_runtime::upsertMarker(transaction, tenantId, "provider", providerId,
                                                 "verify",
                                                 rows.front()[0].as<std::int64_t>().value_or(1));
    co_return true;
}

} // namespace service::provider_verification
