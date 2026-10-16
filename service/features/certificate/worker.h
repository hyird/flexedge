#pragma once

#include <ruvia/core/task.h>
#include <ruvia/core/task_scope.h>
#include <ruvia/core/timer.h>

#include <chrono>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "service/features/background/task_context.h"
#include "service/features/certificate/acme.h"
#include "service/features/certificate/acme_account.h"
#include "service/features/certificate/attempt_detail.h"
#include "service/features/certificate/certificate_inspection.h"
#include "service/features/certificate/certificate_request.h"
#include "service/features/certificate/issuance_state.h"
#include "service/features/certificate/maintenance.h"
#include "service/features/certificate/persistence.h"
#include "service/features/certificate/task.h"
#include "service/features/certificate/work_loader.h"
#include "service/features/dns_sync/queue.h"
#include "service/features/outbound_http/error.h"
#include "service/features/sync_runtime/state.h"
#include "service/features/sync_runtime/worker.h"
#include "service/utils/secret.h"

namespace service::certificate_issuance {

namespace worker_detail {

// One attempt polls the ACME server for at most this long after its claim,
// well inside the lease; a still-pending order resumes on the next attempt.
inline constexpr std::chrono::seconds polling_window{40};
inline constexpr auto lease_renewal_interval = service::sync_runtime::lease_duration / 3;
inline constexpr std::chrono::seconds lease_renewal_timeout{10};

// Issuance runs in phases across attempts, each resumed from the progress
// stored with the certificate:
//  1. new order: create the order; publish the DNS-01 records and park the
//     task until the DNS zone's sync task has applied them;
//  2. validation (resumed by the DNS result): answer the challenges and poll
//     the authorizations; retire the records and store the certificate key;
//  3. finalize: finalize the order, poll it, download and persist the chain.
class issuance final {
public:
    issuance(service::background::task_context& context, service::attempt_trace::recorder& trace,
        const certificate_task& task, certificate_attempt& attempt)
        : context_(context),
          trace_(trace),
          task_(task),
          attempt_(attempt),
          lease_(service::sync_runtime::make_running_lease(task.tenant_id_, task.id_, task.version_,
              context.lease_owner(), task.claimed_at_, service::sync_runtime::marker_resource_type::certificate,
              task.certificate_id_)),
          deadline_(task.claimed_at_ + polling_window) {}

    ruvia::task<void> run() {
        using service::attempt_trace::step_code;
        auto prepare_step = trace_.begin_step(step_code::prepare);
        auto work = co_await load_work(context_, task_);
        attempt_.provider_ = work.provider_;
        attempt_.domains_ = work.domains_;
        if (!co_await await_challenge_dns(context_, trace_, task_, lease_, work, attempt_)) {
            co_return;
        }
        if (work.eab_) {
            trace_.register_secret(work.eab_->key_id_);
            trace_.register_secret(work.eab_->hmac_key_.view());
        }
        if (work.account_email_) {
            trace_.register_secret(*work.account_email_);
        }
        const auto settings = settings_for_provider(work.provider_);
        if (!settings) {
            throw acme_error("不支持的证书供应商", true);
        }
        trace_.log(service::attempt_trace::log_level::info,
            "证书供应商 " + work.provider_ + "，域名 " + std::to_string(work.domains_.size()) + " 个");
        prepare_step.complete();

        acme_client client(*settings, trace_, {.tenant_id_ = work.tenant_id_, .account_id_ = work.provider_id_});
        attempt_.stage_ = certificate_stage::account;
        auto account_step = trace_.begin_step(step_code::account);
        const auto account_email =
            work.account_email_ ? std::optional<std::string_view>{*work.account_email_} : std::nullopt;
        const auto account = co_await ensure_acme_account(context_, work.tenant_id_, work.provider_id_,
            work.provider_revision_, account_email, client, work.eab_);
        client.use_account(account);
        account_step.complete();

        auto progress = std::move(work.progress_);
        if (progress && progress->account_url_ != account.account_url_) {
            trace_.log(service::attempt_trace::log_level::warning, "ACME 账户已变化，重新创建订单");
            progress.reset();
        }
        if (!progress) {
            progress = co_await start_order(client, work, account.account_url_);
            if (!progress) {
                co_return;
            }
        } else {
            trace_.log(service::attempt_trace::log_level::info,
                "继续上次的订单（" + std::string(issuance_phase_name(progress->phase_)) + " 阶段）");
        }
        if (progress->phase_ == issuance_phase::validation) {
            co_await validate(client, *progress);
            progress->phase_ = issuance_phase::finalize;
            progress->authorizations_.clear();
            progress->private_key_envelope_ =
                service::utils::seal_secret(detail::private_key_pem(detail::generate_rsa_key().get()));
            if (!co_await finish_validation(work, *progress)) {
                co_return;
            }
        }
        co_await finalize(client, work, *progress);
    }

private:
    // Creates the order. Returns the progress to continue with in this
    // attempt, or nullopt when the attempt ends here (the task is parked on
    // the DNS sync, or the issuance revision is gone).
    ruvia::task<std::optional<issuance_state>> start_order(acme_client& client, const certificate_work& work,
        std::string_view account_url) {
        using service::attempt_trace::step_code;
        attempt_.stage_ = certificate_stage::order;
        if (!co_await mark_issuance_started(context_, task_, lease_)) {
            co_return std::nullopt;
        }
        auto order_step = trace_.begin_step(step_code::order);
        const auto order = co_await client.create_order(context_, work.domains_);
        order_step.complete();

        attempt_.stage_ = certificate_stage::authorization;
        auto authorization_step = trace_.begin_step(step_code::authorization);
        issuance_state progress{.revision_ = task_.version_,
            .phase_ = issuance_phase::validation,
            .account_url_ = std::string(account_url),
            .order_url_ = order.url_};
        std::vector<service::dns_sync::challenge_record_spec> records;
        for (const auto& url : order.authorizations_) {
            const auto authorization = co_await client.load_authorization(context_, url);
            if (authorization.status_ == "valid") {
                trace_.log(service::attempt_trace::log_level::info, authorization.identifier_ + " 的授权已有效");
                continue;
            }
            if (authorization.status_ == "invalid") {
                throw acme_error(trace_.redact_error(authorization.error_.empty()
                                                         ? "ACME 授权已失效"
                                                         : "ACME 授权失败：" + authorization.error_),
                    false);
            }
            auto record = client.challenge_record(authorization);
            records.push_back({.name_ = std::move(record.name_), .content_ = std::move(record.content_)});
            progress.authorizations_.push_back(
                {.authorization_url_ = url, .challenge_url_ = authorization.dns_challenge_->url_});
        }
        trace_.log(service::attempt_trace::log_level::info,
            records.empty() ? std::string("授权均已有效，跳过 DNS 验证")
                            : "需要完成 " + std::to_string(records.size()) + " 个 DNS-01 验证");
        authorization_step.complete();
        if (records.empty()) {
            trace_.begin_step(step_code::dns_challenge).skip("授权均已有效");
            trace_.begin_step(step_code::validation).skip("授权均已有效");
            progress.phase_ = issuance_phase::finalize;
            progress.private_key_envelope_ =
                service::utils::seal_secret(detail::private_key_pem(detail::generate_rsa_key().get()));
            if (!co_await enter_finalize(context_, task_, lease_, work, progress)) {
                co_return std::nullopt;
            }
            co_return progress;
        }
        attempt_.stage_ = certificate_stage::dns_challenge;
        auto challenge_step = trace_.begin_step(step_code::dns_challenge, work.zone_domain_);
        co_await publish_challenges(context_, trace_, challenge_step, task_, lease_, work, progress, records,
            attempt_);
        co_return std::nullopt;
    }

    // Answers the published challenges and polls their authorizations.
    ruvia::task<void> validate(acme_client& client, const issuance_state& progress) {
        using service::attempt_trace::step_code;
        attempt_.stage_ = certificate_stage::validation;
        auto validation_step = trace_.begin_step(step_code::validation);
        for (const auto& pending : progress.authorizations_) {
            const auto authorization = co_await client.load_authorization(context_, pending.authorization_url_);
            if (authorization.status_ == "pending") {
                co_await client.respond_challenge(context_, pending.challenge_url_);
            }
        }
        for (const auto& pending : progress.authorizations_) {
            co_await client.wait_for_authorization(context_, pending.authorization_url_, deadline_);
        }
        validation_step.complete();
    }

    // Retires the challenge records and stores the finalize progress.
    ruvia::task<bool> finish_validation(const certificate_work& work, const issuance_state& progress) {
        attempt_.stage_ = certificate_stage::cleanup;
        auto cleanup_step = trace_.begin_step(service::attempt_trace::step_code::cleanup);
        if (!co_await enter_finalize(context_, task_, lease_, work, progress)) {
            co_return false;
        }
        trace_.log(service::attempt_trace::log_level::info, "已移除 DNS 验证记录");
        cleanup_step.complete();
        co_return true;
    }

    ruvia::task<void> finalize(acme_client& client, const certificate_work& work, const issuance_state& progress) {
        using service::attempt_trace::step_code;
        attempt_.stage_ = certificate_stage::finalize;
        auto finalize_step = trace_.begin_step(step_code::finalize);
        service::utils::sensitive_string private_key(service::utils::open_secret(progress.private_key_envelope_));
        auto order = co_await client.load_order(context_, progress.order_url_);
        if (order.status_ == "invalid") {
            throw acme_error(trace_.redact_error(order.error_.empty() ? "ACME 证书订单已失效"
                                                                      : "ACME 证书订单失败：" + order.error_),
                false);
        }
        if (order.status_ == "ready") {
            const auto key = detail::load_private_key(private_key.view());
            const auto csr = detail::create_certificate_request(key.get(), work.domains_);
            co_await client.finalize_order(context_, order, csr);
        }
        order = co_await client.wait_for_order(context_, progress.order_url_, deadline_);
        finalize_step.complete();

        attempt_.stage_ = certificate_stage::download;
        auto download_step = trace_.begin_step(step_code::download);
        auto chain = co_await client.download_certificate(context_, order.certificate_);
        auto issued = detail::inspect_certificate(std::move(private_key), std::move(chain), work.domains_);
        trace_.log(service::attempt_trace::log_level::info, "证书链与私钥、域名校验通过，有效期至 " + issued.expires_at_);
        download_step.complete();

        attempt_.stage_ = certificate_stage::persist;
        auto persist_step = trace_.begin_step(step_code::persist);
        co_await persist_issued_certificate(context_, trace_, task_, lease_, issued, attempt_);
        persist_step.complete();
    }

    service::background::task_context& context_;
    service::attempt_trace::recorder& trace_;
    const certificate_task& task_;
    certificate_attempt& attempt_;
    const service::sync_runtime::running_marker_lease lease_;
    const std::chrono::steady_clock::time_point deadline_;
};

// The order survives a failure the ACME server or the network may still
// resolve: the next attempt resumes it.
[[nodiscard]] inline bool keeps_progress(const std::exception_ptr& exception) {
    try {
        std::rethrow_exception(exception);
    } catch (const acme_pending&) {
        return true;
    } catch (const service::outbound_http::transport_error&) {
        return true;
    } catch (...) {
        return false;
    }
}

inline ruvia::task<void> process_task(service::background::task_context& context,
    const service::sync_runtime::claimed_marker& marker) {
    const certificate_task task{.id_ = marker.id_,
        .tenant_id_ = marker.tenant_id_,
        .certificate_id_ = marker.resource_id_,
        .version_ = marker.version_,
        .failures_ = marker.failures_,
        .operation_ = std::string(service::sync_runtime::marker_operation_name(marker.operation_)),
        .claimed_at_ = marker.claimed_at_};
    service::attempt_trace::recorder trace(service::attempt_trace::trace_kind::certificate, "web",
        task.claimed_at_);
    certificate_attempt attempt{.operation_ = task.operation_,
        .provider_ = {},
        .domains_ = {},
        .stage_ = certificate_stage::prepare,
        .issued_ = std::nullopt};
    const auto lease = service::sync_runtime::make_running_lease(task.tenant_id_, task.id_, task.version_,
        context.lease_owner(), task.claimed_at_, service::sync_runtime::marker_resource_type::certificate,
        task.certificate_id_);
    ruvia::task_scope operations(context.worker(), {.resource_ = context.pool()});
    service::background::task_context owned(context, operations.get_stop_token());
    bool lease_lost{};
    // Renewal belongs to this attempt on the same Web Worker. A bounded
    // renewal failure cancels its quota waits, HTTP requests and database IO.
    auto maintain_lease = [&]() -> ruvia::task<void> {
        while (co_await ruvia::sleep_for(context.worker(), lease_renewal_interval,
                   owned.get_stop_token()) != ruvia::timer_sleep_result::stop_requested) {
            try {
                auto database = owned.db().with_options({.timeout_ = lease_renewal_timeout});
                if (co_await service::sync_runtime::renew_running_lease(database, lease)) {
                    continue;
                }
            } catch (...) {
                if (owned.get_stop_token().stop_requested()) {
                    co_return;
                }
            }
            lease_lost = true;
            operations.request_stop();
            co_return;
        }
    };
    std::exception_ptr exception;
    try {
        auto database = owned.db().with_options({.timeout_ = lease_renewal_timeout});
        if (!co_await service::sync_runtime::renew_running_lease(database, lease)) {
            co_return;
        }
        operations.spawn(maintain_lease());
        issuance flow(owned, trace, task, attempt);
        co_await flow.run();
    } catch (...) {
        exception = std::current_exception();
    }
    operations.request_stop();
    co_await operations.join();
    if (!context.get_stop_token().stop_requested() && !lease_lost && exception) {
        const auto failure = service::outbound_http::classify_attempt_failure(exception, "证书任务发生未知错误");
        const bool keep_progress = !failure.permanent_ && keeps_progress(exception);
        trace.mark_failure(failure.message_);
        auto step = trace.begin_step(service::attempt_trace::step_code::record_failure);
        trace.log(failure.permanent_ ? service::attempt_trace::log_level::error
                                     : service::attempt_trace::log_level::warning,
            failure.permanent_ ? "永久失败，不再自动重试"
            : keep_progress    ? "将在 15 秒后自动重试并继续当前订单"
                               : "将在 15 秒后自动重试");
        co_await fail_certificate_task(context, trace, task, failure.message_, failure.permanent_, keep_progress,
            attempt.to_json(context.pool()));
        step.complete();
    }
}

}  // namespace worker_detail

inline ruvia::task<void> run_worker(service::background::task_context& context) {
    co_await service::sync_runtime::run_marker_worker(context,
        service::sync_runtime::marker_resource_type::certificate, worker_detail::reconcile_certificate_maintenance,
        worker_detail::process_task);
}

}  // namespace service::certificate_issuance
