#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

#include <ruvia/core/Task.h>
#include <ruvia/web/db/DbTransaction.h>

#include "service/features/sync_runtime/state.h"

namespace service::certificate_issuance {

inline ruvia::Task<std::string> enqueueCertificateRevision(ruvia::DbTransaction& transaction,
                                                           std::string_view tenantId,
                                                           std::string_view certificateId,
                                                           std::int64_t revision,
                                                           std::string_view operation) {
    const auto rows = co_await transaction.query(
        "SELECT id FROM sys_certificate WHERE tenant_id = $1 AND id = $2 AND "
        "issuance_revision = $3 AND deleted_at IS NULL LIMIT 1",
        tenantId, certificateId, revision);
    if (rows.empty()) {
        throw std::runtime_error("证书聚合根不存在或签发版本已变化");
    }
    co_return co_await service::sync_runtime::upsertMarker(transaction, tenantId, "certificate",
                                                           certificateId, operation, revision);
}

} // namespace service::certificate_issuance
