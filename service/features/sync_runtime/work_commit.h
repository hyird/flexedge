#pragma once

#include <ruvia/core/task.h>

#include <string_view>
#include <utility>

#include "service/features/background/queued_work.h"
#include "service/features/background/work_available.h"
#include "service/features/live_resource/notification_outbox.h"

namespace service::sync_runtime {

// Adds the tasks `work` changed to the notification sent after its commit.
inline void add_changed_tasks(service::live_resource::notifications::resource_notification& notification,
    const service::background::queued_work& work) {
    for (const auto& id : work.changed_tasks()) {
        notification.changed(service::live_resource::resource::tasks, id);
    }
}

// Commits a write that may have queued background work: notifies the
// resources in `notification` and the tasks the write changed, then wakes the
// committing worker's own claim loops, so it claims the work it queued first.
template <typename transaction_type, typename context_type>
inline ruvia::task<void> commit_work(transaction_type& transaction, context_type& context,
    service::live_resource::notifications::resource_notification notification,
    const service::background::queued_work& work) {
    add_changed_tasks(notification, work);
    co_await service::live_resource::commit_and_notify(transaction, context, notification);
    service::background::work_available::wake_own_claims(context, work.claims());
}

template <typename transaction_type, typename context_type>
inline ruvia::task<void> commit_work(transaction_type& transaction, context_type& context, std::string_view tenant_id,
    const service::background::queued_work& work) {
    co_await commit_work(transaction, context,
        service::live_resource::notifications::resource_notification(tenant_id), work);
}

template <typename transaction_type, typename context_type>
inline ruvia::task<void> commit_work(transaction_type& transaction, context_type& context,
    std::string_view tenant_id, service::live_resource::resource resource_kind,
    std::string_view resource_id, const service::background::queued_work& work) {
    service::live_resource::notifications::resource_notification notification(tenant_id);
    notification.changed(resource_kind, resource_id);
    co_await commit_work(transaction, context, std::move(notification), work);
}

}  // namespace service::sync_runtime
