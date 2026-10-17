#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/model_json.h>

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <string>

#include "service/features/sync_runtime/state.h"
#include "service/features/website_dns/attempt_detail.h"
#include "service/features/website_dns/mapper.h"

namespace service::website_dns {

inline constexpr std::chrono::minutes check_interval{10};

// Verification timestamps advance on every check; only resolution changes notify views.
[[nodiscard]] inline bool same_resolution(const website_runtime_data& previous, const website_runtime_data& current) {
    return previous.domain_states_.size() == current.domain_states_.size() &&
           std::ranges::all_of(current.domain_states_, [&](const auto& state) {
               const auto found = std::ranges::find(previous.domain_states_, state.id_, &website_domain_runtime_data::id_);
               return found != previous.domain_states_.end() && found->resolution_status_ == state.resolution_status_ &&
                      found->last_error_.value_or("") == state.last_error_.value_or("");
           });
}

template <typename context_type>
inline ruvia::task<void> persist_website_check(context_type& context, service::attempt_trace::recorder& trace,
    const service::sync_runtime::running_marker_lease& lease, const website_runtime_data& output,
    const website_domain_attempt& attempt, bool inspection) {
    const auto json = ruvia::to_json(to_output(output, {.resource_ = context.pool()}), {.resource_ = context.pool()});
    const auto error = attempt.error();
    auto transaction = co_await context.db().begin_transaction();
    service::sync_runtime::running_result_writer writer(transaction, lease, check_interval);
    const auto rows = co_await transaction.query(
        "SELECT website.runtime::text, task.is_ok, task.error FROM sys_website website "
        "JOIN sys_sync_task task ON task.tenant_id = website.tenant_id AND task.resource_type = 'website' "
        "AND task.resource_id = website.id WHERE website.tenant_id = $1 AND website.id = $2 "
        "AND website.revision = $3 AND website.deleted_at IS NULL AND task.id = $4 "
        "AND task.version = $3 AND task.lease_owner = $5 AND task.lease_until > NOW() FOR UPDATE OF website, task",
        lease.marker_.tenant_id_, lease.marker_.resource_id_, lease.marker_.version_, lease.marker_.marker_id_, lease.owner_);
    if (rows.empty()) {
        (void)co_await writer.quiet(service::sync_runtime::quiet_action::release);
        co_await writer.commit(context);
        co_return;
    }
    const auto previous = parse_stored(rows.front()[0].value().value_or("{}"), {.resource_ = context.pool()});
    if (!previous) {
        throw std::runtime_error("网站运行状态损坏");
    }
    const bool unchanged = inspection && same_resolution(*previous, output) &&
                           rows.front()[1].template as<bool>().value_or(false) == error.empty() &&
                           rows.front()[2].value().value_or("") == error;
    (void)co_await transaction.execute(
        "UPDATE sys_website SET runtime = $3::jsonb WHERE tenant_id = $1 AND id = $2",
        lease.marker_.tenant_id_, lease.marker_.resource_id_, std::string_view(json));
    if (unchanged) {
        (void)co_await writer.quiet(service::sync_runtime::quiet_action::inspect);
    } else {
        const auto result = co_await writer.record(error.empty() ? service::sync_runtime::attempt_result::completed()
                                                                 : service::sync_runtime::attempt_result::failed(error),
            attempt.to_json(context.pool()), service::sync_runtime::failure_count::restart);
        if (!result.changed()) {
            throw std::runtime_error("网站检查任务 lease 已失效");
        }
    }
    co_await writer.commit(context, trace);
}

}  // namespace service::website_dns
