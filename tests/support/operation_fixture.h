#pragma once

#include <ruvia/core/task.h>

#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>

#include "service/features/operation/work_origin.h"

namespace test_support {

// What task_service needs from a request context.
template <typename handle_type>
struct read_context final {
    handle_type handle_;
    std::pmr::memory_resource* pool_;

    handle_type& db() {
        return handle_;
    }
    [[nodiscard]] std::pmr::memory_resource* pool() const {
        return pool_;
    }
};

// A system task with one subtask doing `kind` work on a resource at
// `version`; `marker_id`, when given, names the sys_sync_task row it owns.
template <typename database_type>
ruvia::task<service::operation::work_owner> own_work(database_type& database, std::string_view tenant_id,
    service::operation::action action, std::string_view target_type, std::string_view target_id,
    service::operation::subtask_kind kind, std::string_view resource_type, std::string_view resource_id,
    std::int64_t version, std::string_view marker_id = {}) {
    auto origin = service::operation::work_origin::system(action, std::string(target_type), std::string(target_id));
    const auto subtask = co_await origin.require_subtask(database, tenant_id, kind, resource_type, resource_id, version);
    if (!marker_id.empty()) {
        (void)co_await database.execute(
            "UPDATE sys_sync_task SET subtask_id = $3::uuid WHERE tenant_id = $1 AND id = $2", tenant_id, marker_id,
            std::string_view(subtask.value_or("")));
    }
    co_return service::operation::work_owner{.operation_id_ = origin.operation_id().value_or(""),
        .subtask_id_ = subtask.value_or("")};
}

}  // namespace test_support
