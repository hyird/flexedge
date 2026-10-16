#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/db/db_handle.h>

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "service/features/live_resource/notifications.h"

namespace test_support {

// A worker context whose resource notifications are recorded instead of
// sent to Redis. Each send also records whether the notified attempts are
// already visible on the autocommit handle, i.e. committed before the send.
// Own claim wakeups after a commit are recorded instead of signalled.
struct recorded_notifications final {
    ruvia::db_handle handle_;
    std::vector<service::live_resource::notifications::resource_notification> sent_{};
    std::vector<bool> committed_{};
    std::vector<std::string> own_claims_{};
    std::pmr::memory_resource* pool_{std::pmr::new_delete_resource()};

    ruvia::db_handle db() {
        return handle_;
    }
    [[nodiscard]] std::pmr::memory_resource* pool() const {
        return pool_;
    }
    std::nullptr_t redis() {
        return nullptr;
    }
    ruvia::task<void> record_committed_notification( const service::live_resource::notifications::resource_notification& value) {
        bool committed = true;
        for (const auto& attempt : value.task_attempts_) {
            const auto rows = co_await handle_.query("SELECT count(*) FROM sys_sync_event WHERE id = $1",
                attempt.notice_.event_id_);
            committed = committed && rows.front()[0].template as<std::int64_t>().value_or(0) == 1;
        }
        committed_.push_back(committed);
        sent_.push_back(value);
    }
    void record_own_claims(std::span<const std::string_view> work_types) {
        own_claims_.insert(own_claims_.end(), work_types.begin(), work_types.end());
    }
};

}  // namespace test_support
