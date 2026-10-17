#pragma once

#include <ruvia/core/task.h>

#include <cstdint>
#include <exception>
#include <string>

#include "service/features/background/task_context.h"
#include "service/features/outbound_http/error.h"
#include "service/features/sync_runtime/state.h"
#include "service/features/sync_runtime/work_commit.h"
#include "service/features/sync_runtime/worker.h"
#include "service/features/website_dns/maintenance.h"
#include "service/features/website_dns/runtime.h"

namespace service::website_dns {

namespace detail {

// Re-creates the probe marker of websites whose current revision lost it.
inline ruvia::task<void> reconcile_markers(service::background::task_context& context) {
    ruvia::db_query candidates(context.pool());
    candidates.select({candidates.column("tenant_id", "website"), candidates.column("id", "website"),
                          candidates.column("revision", "website")})
        .from("sys_website", "website")
        .where(candidates.sql(
            "website.deleted_at IS NULL AND NOT EXISTS (SELECT 1 FROM sys_sync_task marker "
            "WHERE marker.resource_type = 'website' AND marker.tenant_id = website.tenant_id "
            "AND marker.resource_id = website.id AND marker.version = website.revision)"))
        .limit(64);
    const auto missing = co_await context.db().query(candidates);
    for (const auto& row : missing) {
        const auto tenant_id = std::string(row[0].value().value_or(""));
        const auto website_id = std::string(row[1].value().value_or(""));
        const auto version = row[2].as<std::int64_t>().value_or(1);
        auto transaction = co_await context.db().begin_transaction();
        ruvia::db_query aggregate(context.pool());
        aggregate.select(aggregate.column("id"))
            .from("sys_website")
            .where(aggregate.sql({"tenant_id = ", " AND id = ", " AND revision = ",
                                     " AND deleted_at IS NULL"},
                {aggregate.value(tenant_id), aggregate.value(website_id), aggregate.value(version)}))
            .limit(1)
            .lock({.mode_ = ruvia::db_row_lock::update});
        const auto locked = co_await transaction.query(aggregate);
        if (locked.empty() || co_await service::sync_runtime::marker_version_exists(transaction, tenant_id,
                                  service::sync_runtime::marker_resource_type::website, website_id, version)) {
            co_await transaction.commit();
            continue;
        }
        service::background::queued_work work;
        (void)co_await service::sync_runtime::upsert_marker(transaction, tenant_id,
            service::sync_runtime::marker_resource_type::website, website_id,
            service::sync_runtime::marker_operation::apply, version, work);
        co_await service::sync_runtime::commit_work(transaction, context, tenant_id, work);
    }
}

inline ruvia::task<void> process_marker(service::background::task_context& context,
    const service::sync_runtime::claimed_marker& marker, bool inspection = false) {
    service::attempt_trace::recorder trace(service::attempt_trace::trace_kind::website_domains, "web",
        marker.claimed_at_);
    const auto lease = service::sync_runtime::make_running_lease(marker.tenant_id_, marker.id_, marker.version_,
        context.lease_owner(), marker.claimed_at_, service::sync_runtime::marker_resource_type::website,
        marker.resource_id_);
    website_domain_attempt attempt;
    std::exception_ptr exception;
    try {
        co_await probe_website(context, trace, marker.resource_id_, lease, marker.version_, attempt, inspection);
    } catch (...) {
        exception = std::current_exception();
    }
    if (!context.get_stop_token().stop_requested() && exception) {
        const auto failure =
            service::outbound_http::classify_attempt_failure(exception, "网站同步发生未知错误");
        auto step = service::sync_runtime::record_attempt_failure_step(trace, failure.message_);
        co_await fail_website_task(context, trace, lease, failure.message_, attempt.to_json(context.pool()));
        step.complete();
    }
}

}  // namespace detail

inline ruvia::task<void> run_worker(service::background::task_context& context) {
    auto maintain = [next_repair = std::chrono::steady_clock::time_point{}](service::background::task_context& owned) mutable -> ruvia::task<void> {
        if (std::chrono::steady_clock::now() >= next_repair) {
            co_await detail::reconcile_markers(owned);
            next_repair = std::chrono::steady_clock::now() + service::sync_runtime::reconciliation_interval;
        }
        const auto limit = service::sync_runtime::describe(service::sync_runtime::marker_resource_type::website).claims_per_round_;
        for (std::size_t count = 0; count < limit && !owned.get_stop_token().stop_requested(); ++count) {
            const auto inspection = co_await detail::claim_due_check(owned);
            if (!inspection) {
                break;
            }
            co_await detail::process_marker(owned, *inspection, true);
        }
    };
    co_await service::sync_runtime::run_marker_worker(context, service::sync_runtime::marker_resource_type::website, maintain, [](service::background::task_context& owned, const service::sync_runtime::claimed_marker& marker) { return detail::process_marker(owned, marker); }, {}, std::chrono::seconds(15));
}

}  // namespace service::website_dns
