#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/db/db_transaction.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

#include "service/features/sync_runtime/state.h"

namespace service::certificate_issuance {

inline ruvia::task<std::string>
enqueue_certificate_revision(ruvia::db_transaction& transaction, std::string_view tenant_id,
    std::string_view certificate_id, std::int64_t revision,
    service::sync_runtime::marker_operation operation) {
    const auto rows = co_await transaction.query(
        "SELECT id FROM sys_certificate WHERE tenant_id = $1 AND id = $2 AND "
        "issuance_revision = $3 AND deleted_at IS NULL LIMIT 1",
        tenant_id, certificate_id, revision);
    if (rows.empty()) {
        throw std::runtime_error("证书聚合根不存在或签发版本已变化");
    }
    co_return co_await service::sync_runtime::upsert_marker(
        transaction, tenant_id, service::sync_runtime::marker_resource_type::certificate,
        certificate_id, operation, revision);
}

}  // namespace service::certificate_issuance
