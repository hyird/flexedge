#pragma once

#include <ruvia/core/task.h>

#include <cstdint>
#include <exception>
#include <string>

#include "service/features/background/task_context.h"
#include "service/features/sync_runtime/state.h"
#include "service/features/sync_runtime/work_commit.h"
#include "service/features/sync_runtime/worker.h"
#include "service/features/website_dns/runtime.h"

namespace service::website_dns {

namespace detail {

// Re-creates the probe marker of websites whose current revision lost it.
inline ruvia::task<void> reconcile_markers(service::background::task_context& context) {
    auto db = context.db();
    const auto missing = co_await db.query(
        "SELECT website.tenant_id, website.id, website.revision FROM sys_website website WHERE "
        "website.deleted_at IS NULL AND NOT EXISTS (SELECT 1 FROM sys_sync_task marker WHERE "
        "marker.tenant_id = website.tenant_id AND marker.resource_type = 'website' AND "
        "marker.resource_id = website.id AND marker.version = website.revision) ORDER BY "
        "website.updated_at ASC LIMIT 64");
    for (const auto& row : missing) {
        service::background::queued_work work;
        auto transaction = co_await db.begin_transaction();
        const auto tenant_id = std::string(row[0].value().value_or(""));
        const auto website_id = std::string(row[1].value().value_or(""));
        const auto revision = row[2].as<std::int64_t>().value_or(1);
        const auto locked = co_await transaction.query(
            "SELECT revision FROM sys_website WHERE tenant_id = $1 AND id = $2 AND revision = $3 "
            "AND deleted_at IS NULL LIMIT 1 FOR UPDATE",
            tenant_id, website_id, revision);
        if (!locked.empty()) {
            (void)co_await service::sync_runtime::upsert_marker(transaction, tenant_id,
                service::sync_runtime::marker_resource_type::website, website_id,
                service::sync_runtime::marker_operation::apply, revision, work);
        }
        co_await service::sync_runtime::commit_work(transaction, context, tenant_id, work);
    }
}

inline ruvia::task<void> process_marker(service::background::task_context& context,
    const service::sync_runtime::claimed_marker& marker) {
    service::attempt_trace::recorder trace(service::attempt_trace::trace_kind::website_domains, "web",
        marker.claimed_at_);
    const auto lease = service::sync_runtime::make_running_lease(marker.tenant_id_, marker.id_, marker.version_,
        context.lease_owner(), marker.claimed_at_, service::sync_runtime::marker_resource_type::website,
        marker.resource_id_);
    website_domain_attempt attempt;
    std::string marker_error;
    try {
        co_await probe_website(context, trace, marker.resource_id_, lease, marker.version_, attempt);
    } catch (const std::exception& error) {
        marker_error = error.what();
    } catch (...) {
        marker_error = "网站同步发生未知错误";
    }
    if (!context.get_stop_token().stop_requested() && !marker_error.empty()) {
        trace.mark_failure(marker_error);
        auto step = trace.begin_step(service::attempt_trace::step_code::record_failure);
        trace.log(service::attempt_trace::log_level::warning, "将在 15 秒后自动重试");
        co_await fail_website_task(context, trace, lease, marker_error, attempt.to_json(context.pool()));
        step.complete();
    }
}

}  // namespace detail

inline ruvia::task<void> run_worker(service::background::task_context& context) {
    co_await service::sync_runtime::run_marker_worker(context, service::sync_runtime::marker_resource_type::website,
        detail::reconcile_markers, detail::process_marker);
}

}  // namespace service::website_dns
