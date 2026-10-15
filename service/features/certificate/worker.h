#pragma once

#include <ruvia/core/task.h>
#include <ruvia/core/timer.h>

#include <chrono>
#include <cstddef>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "service/features/background/marker_worker_loop.h"
#include "service/features/background/worker_pool.h"
#include "service/features/certificate/acme_account.h"
#include "service/features/certificate/attempt_detail.h"
#include "service/features/certificate/maintenance.h"
#include "service/features/certificate/persistence.h"
#include "service/features/certificate/task.h"
#include "service/features/certificate/task_loader.h"
#include "service/features/certificate/work_loader.h"
#include "service/features/sync_runtime/error.h"
#include "service/features/sync_runtime/state.h"

namespace service::certificate_issuance {

namespace worker_detail {

inline constexpr std::chrono::seconds idle_poll_interval{2};
inline constexpr std::chrono::seconds lease_recovery_interval{15};
inline constexpr std::chrono::minutes reconciliation_interval{15};
inline constexpr std::size_t max_jobs_per_tick{4};

inline ruvia::task<void> recover_stale_markers(service::background::worker_context& context) {
    co_await service::sync_runtime::recover_stale_running(
        context, service::sync_runtime::marker_resource_type::certificate);
    co_return;
}

inline ruvia::task<void> execute(service::background::worker_context& context,
    service::attempt_trace::recorder& trace, const certificate_task& task, certificate_attempt& attempt) {
    using service::attempt_trace::step_code;
    const auto lease = service::sync_runtime::make_running_lease(
        task.tenant_id_, task.id_, task.version_, context.lease_owner(), task.claimed_at_,
        service::sync_runtime::marker_resource_type::certificate, task.certificate_id_);
    auto prepare_step = trace.begin_step(step_code::prepare);
    auto work = co_await load_work(context, task);
    attempt.provider_ = work.provider_;
    attempt.domains_ = work.domains_;
    if (work.eab_) {
        trace.register_secret(work.eab_->key_id_);
        trace.register_secret(work.eab_->hmac_key_.view());
    }
    if (work.account_email_) {
        trace.register_secret(*work.account_email_);
    }
    const auto settings = settings_for_provider(work.provider_);
    if (!settings) {
        throw acme_error("不支持的证书供应商", true);
    }
    trace.log(service::attempt_trace::log_level::info,
        "证书供应商 " + work.provider_ + "，域名 " + std::to_string(work.domains_.size()) + " 个");
    prepare_step.complete();
    acme_client client(*settings, trace);
    const auto account_email =
        work.account_email_ ? std::optional<std::string_view>{*work.account_email_} : std::nullopt;
    attempt.stage_ = certificate_stage::account;
    auto account_step = trace.begin_step(step_code::account);
    auto account =
        co_await ensure_acme_account(context, work.tenant_id_, work.provider_id_, work.provider_revision_,
            account_email, client, work.eab_);
    account_step.complete();
    attempt.stage_ = certificate_stage::order;
    if (!co_await mark_issuance_started(context, task, lease)) {
        co_return;
    }

    const dns_challenge_config_view dns_config{.dns_zone_id_ = work.dns_zone_id_,
        .zone_domain_ = work.zone_domain_,
        .certificate_id_ = task.certificate_id_,
        .minimum_record_ttl_ = work.dns_minimum_record_ttl_,
        .owner_ = task.owner_};
    auto issued = co_await client.issue(context, account.private_key_pem_.view(), account.account_url_,
        work.domains_, dns_config, lease, attempt.stage_);
    attempt.stage_ = certificate_stage::persist;
    auto persist_step = trace.begin_step(step_code::persist);
    co_await persist_issued_certificate(context, trace, task, lease, issued, attempt);
    persist_step.complete();
    co_return;
}

inline ruvia::task<void> process_claimed_task(service::background::worker_context& context,
    const certificate_task& task) {
    service::attempt_trace::recorder trace(service::attempt_trace::trace_kind::certificate, "certificate",
        task.claimed_at_);
    certificate_attempt attempt{.operation_ = task.operation_,
        .provider_ = {},
        .domains_ = {},
        .stage_ = certificate_stage::prepare,
        .issued_ = std::nullopt};
    std::string task_error;
    bool permanent = false;
    try {
        co_await execute(context, trace, task, attempt);
    } catch (const acme_error& error) {
        task_error = error.what();
        permanent = error.permanent();
    } catch (const std::exception& error) {
        task_error = error.what();
    } catch (...) {
        task_error = "证书任务发生未知错误";
    }
    if (!task_error.empty()) {
        trace.mark_failure(task_error);
        auto step = trace.begin_step(service::attempt_trace::step_code::record_failure);
        trace.log(permanent ? service::attempt_trace::log_level::error : service::attempt_trace::log_level::warning,
            permanent ? "永久失败，不再自动重试" : "将在 15 秒后自动重试");
        co_await fail_certificate_task(context, trace, task, task_error, permanent,
            attempt.to_json(context.pool()));
        step.complete();
    }
    co_return;
}

inline ruvia::task<void> process_available_tasks(service::background::worker_context& context,
    std::size_t& processed) {
    for (; processed < max_jobs_per_tick; ++processed) {
        const auto task = co_await claim(context);
        if (!task) {
            break;
        }
        co_await process_claimed_task(context, *task);
    }
    co_return;
}

inline ruvia::task<void> run_maintenance(service::background::worker_context& context,
    std::chrono::steady_clock::time_point& next_lease_recovery,
    std::chrono::steady_clock::time_point& next_reconciliation) {
    if (std::chrono::steady_clock::now() >= next_lease_recovery) {
        co_await recover_stale_markers(context);
        next_lease_recovery = std::chrono::steady_clock::now() + lease_recovery_interval;
    }
    if (std::chrono::steady_clock::now() >= next_reconciliation) {
        co_await reconcile_certificate_maintenance(context);
        next_reconciliation = std::chrono::steady_clock::now() + reconciliation_interval;
    }
    co_return;
}

inline ruvia::task<void> run(service::background::worker_context& context) {
    co_await service::background::run_marker_worker_loop(
        context, idle_poll_interval, "Certificate worker failure: ", "未知证书同步错误",
        run_maintenance, process_available_tasks, service::sync_runtime::worker_error);
    co_return;
}

}  // namespace worker_detail

inline ruvia::task<void> run_worker(service::background::worker_context& context) {
    co_await worker_detail::run(context);
}

}  // namespace service::certificate_issuance
