#pragma once

#include <ruvia/core/task.h>

#include <array>
#include <exception>
#include <string>
#include <string_view>
#include <utility>

#include "service/features/background/task_context.h"
#include "service/features/outbound_http/error.h"
#include "service/features/provider_verification/persistence.h"
#include "service/features/provider_verification/task.h"
#include "service/features/provider_verification/verification.h"
#include "service/features/sync_runtime/state.h"
#include "service/features/sync_runtime/worker.h"

namespace service::provider_verification {

namespace detail {

// The provider row a verification runs against, claimed with its marker.
inline constexpr std::array<std::string_view, 7> claimed_provider_columns{
    "kind", "provider", "name", "account_id", "revision", "config", "runtime"};

inline ruvia::task<void> execute(service::background::task_context& context,
    service::attempt_trace::recorder& trace, const verification_task& task, provider_attempt& attempt) {
    auto load_step = trace.begin_step(service::attempt_trace::step_code::load_provider, task.name_);
    const auto current_runtime = co_await load_current_runtime(context, task);
    if (!current_runtime) {
        load_step.skip("供应商配置已更新，本次让位于新的检测");
        co_return;
    }
    load_step.complete();
    verification_result result;
    if (task.kind_ == "dns") {
        result = co_await verify_dns(context, trace, task, attempt);
    } else if (task.kind_ == "certificate") {
        result = co_await verify_certificate(context, trace, task, *current_runtime, attempt);
    } else {
        throw service::outbound_http::attempt_error("供应商类型无效", true);
    }
    auto save_step = trace.begin_step(service::attempt_trace::step_code::save_result);
    co_await complete_verification(context, trace, task, std::move(result), attempt.to_json(context.pool()));
    save_step.complete();
}

inline ruvia::task<void> process_task(service::background::task_context& context,
    const verification_task& task) {
    service::attempt_trace::recorder trace(service::attempt_trace::trace_kind::provider, "web",
        task.claimed_at_);
    provider_attempt attempt(task.provider_, task.kind_);
    std::exception_ptr exception;
    try {
        co_await execute(context, trace, task, attempt);
    } catch (...) {
        exception = std::current_exception();
    }
    if (!context.get_stop_token().stop_requested() && exception) {
        const auto failure = service::outbound_http::classify_attempt_failure(exception, "供应商检测发生未知错误");
        auto step = service::sync_runtime::record_attempt_failure_step(
            trace, failure.message_, failure.permanent_, "永久失败，供应商标记为无效，不再自动重试");
        co_await fail_verification(context, trace, task, failure.message_, failure.permanent_,
            attempt.to_json(context.pool()));
        step.complete();
    }
    co_return;
}

inline ruvia::task<void> process_marker(service::background::task_context& context,
    const service::sync_runtime::claimed_marker& marker) {
    const auto& provider = marker.columns();
    co_await process_task(context,
        verification_task{.id_ = marker.id_,
            .tenant_id_ = marker.tenant_id_,
            .provider_id_ = marker.resource_id_,
            .kind_ = std::string(provider["kind"].value().value_or("")),
            .provider_ = std::string(provider["provider"].value().value_or("")),
            .name_ = std::string(provider["name"].value().value_or("")),
            .account_id_ = std::string(provider["account_id"].value().value_or("")),
            .provider_revision_ = provider["revision"].as<std::int64_t>().value_or(0),
            .generation_ = marker.version_,
            .failures_ = marker.failures_,
            .config_json_ = std::string(provider["config"].value().value_or("{}")),
            .runtime_json_ = std::string(provider["runtime"].value().value_or("{}")),
            .claimed_at_ = marker.claimed_at_});
}

}  // namespace detail

inline ruvia::task<void> run_worker(service::background::task_context& context) {
    co_await service::sync_runtime::run_marker_worker(context, service::sync_runtime::marker_resource_type::provider,
        detail::reconcile, detail::process_marker, detail::claimed_provider_columns);
}

}  // namespace service::provider_verification
