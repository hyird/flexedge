#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/model_json.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

#include "service/features/background/worker_pool.h"
#include "service/features/sync_runtime/state.h"
#include "service/features/website_config/mapper.h"
#include "service/features/website_dns/attempt_detail.h"
#include "service/features/website_dns/mapper.h"
#include "service/features/website_dns/probe.h"

namespace service::website_dns {

inline ruvia::task<void>
complete_stale_website_task(service::background::worker_context& context,
    const service::sync_runtime::running_marker_lease& marker,
    bool revision_changed) {
    auto completion = co_await context.db().begin_transaction();
    if (revision_changed) {
        (void)co_await service::sync_runtime::release_running(completion, marker);
    } else {
        (void)co_await service::sync_runtime::complete_running(completion, marker);
    }
    co_await completion.commit();
    co_return;
}

inline ruvia::task<website_runtime_data>
probe_website_domains(service::background::worker_context& context, service::attempt_trace::recorder& trace,
    const service::website_config::website_config_data& config,
    std::string_view expected_target, std::string_view checked_at,
    const service::sync_runtime::running_marker_lease& marker, website_domain_attempt& attempt) {
    website_runtime_data output;
    auto& domain_states = output.domain_states_;
    auto db = context.db();
    for (const auto& domain : config.domains_) {
        auto step = trace.begin_step(service::attempt_trace::step_code::check_domain, domain.hostname_);
        if (!co_await service::sync_runtime::renew_running_lease(db, marker)) {
            throw std::runtime_error("网站同步标记 lease 已失效");
        }
        auto& state = domain_states.emplace_back();
        state.id_ = domain.id_;
        state.last_verified_at_ = std::string(checked_at);
        website_domain_check check{.hostname_ = domain.hostname_,
            .query_name_ = probe_name(domain.hostname_, domain.id_),
            .resolved_ = {},
            .status_ = {},
            .error_ = {}};
        try {
            auto result =
                co_await probe_cname(context, trace, domain.hostname_, expected_target, domain.id_);
            state.resolution_status_ = result.matched_ ? "verified" : "invalid";
            if (!result.matched_) {
                const auto observed =
                    result.observed_target_.empty() ? "未查询到 CNAME" : result.observed_target_;
                state.last_error_ = "期望 " + std::string(expected_target) + "，实际 " + observed;
                trace.log(service::attempt_trace::log_level::warning, *state.last_error_);
            } else {
                trace.log(service::attempt_trace::log_level::info, "CNAME 指向 " + result.observed_target_);
            }
            check.query_name_ = std::move(result.query_name_);
            check.resolved_ = std::move(result.answers_);
            step.complete();
        } catch (const std::exception& error) {
            state.resolution_status_ = "unverified";
            state.last_error_ = trace.redact_error(error.what());
            step.fail(error.what());
        }
        check.status_ = *state.resolution_status_;
        check.error_ = state.last_error_.value_or("");
        attempt.record_check(std::move(check));
    }
    co_return output;
}

inline ruvia::task<void> probe_website(service::background::worker_context& context,
    service::attempt_trace::recorder& trace, const std::string& website_id,
    const service::sync_runtime::running_marker_lease& marker,
    std::int64_t expected_revision, website_domain_attempt& attempt) {
    auto load_step = trace.begin_step(service::attempt_trace::step_code::load_website);
    auto db = context.db();
    const auto rows = co_await db.query(
        "SELECT website.config::text, website.runtime::text, cluster.hostname_prefix || '.' || "
        "zone.domain, website.revision, website.deleted_at IS NOT NULL, TO_CHAR(NOW(), "
        "'YYYY-MM-DD\"T\"HH24:MI:SS.USOF') FROM sys_website website INNER JOIN sys_cluster "
        "cluster ON cluster.tenant_id = website.tenant_id AND cluster.id = "
        "website.cluster_id INNER JOIN sys_dns_zone zone ON zone.tenant_id = "
        "cluster.tenant_id AND zone.id = cluster.dns_zone_id WHERE website.id = $1 AND "
        "website.tenant_id = $2 LIMIT 1",
        website_id, marker.marker_.tenant_id_);
    const bool revision_changed =
        !rows.empty() && rows.front()[3].as<std::int64_t>().value_or(0) != expected_revision;
    const bool resource_deleted = rows.empty() || rows.front()[4].as<bool>().value_or(false);
    if (resource_deleted) {
        load_step.skip("网站已删除");
        auto completion = co_await db.begin_transaction();
        (void)co_await service::sync_runtime::complete_running(completion, marker);
        co_await completion.commit();
        co_return;
    }
    if (rows.empty() || revision_changed) {
        load_step.skip("网站配置已更新，本次让位于新的检查");
        co_await complete_stale_website_task(context, marker, revision_changed);
        co_return;
    }

    const auto config = service::website_config::parse_stored(rows.front()[0].value().value_or("{}"),
        {.resource_ = context.pool()});
    const auto runtime =
        parse_stored(rows.front()[1].value().value_or("{}"), {.resource_ = context.pool()});
    if (!config || !runtime) {
        throw std::runtime_error("网站运行状态损坏");
    }

    const auto expected_target = rows.front()[2].value().value_or("");
    const auto checked_at = rows.front()[5].value().value_or("");
    attempt.start(std::string(expected_target), config->domains_.size());
    trace.log(service::attempt_trace::log_level::info, "期望 CNAME 目标 " + std::string(expected_target) + "，域名 " +
                                                           std::to_string(config->domains_.size()) + " 个");
    load_step.complete();
    auto output = co_await probe_website_domains(context, trace, *config, expected_target, checked_at, marker,
        attempt);
    auto save_step = trace.begin_step(service::attempt_trace::step_code::save_runtime);

    const auto json = ruvia::to_json(to_output(output, {.resource_ = context.pool()}),
        {.resource_ = context.pool()});
    auto completion = co_await db.begin_transaction();
    const auto updated = co_await completion.execute(
        "UPDATE sys_website SET runtime = $2::jsonb WHERE id = $1 AND revision = $3 AND "
        "tenant_id = $4 AND deleted_at IS NULL",
        website_id, std::string_view(json), expected_revision, marker.marker_.tenant_id_);
    service::sync_runtime::running_result_transition result_transition;
    if (updated.affected_rows() == 0) {
        (void)co_await service::sync_runtime::release_running(completion, marker);
    } else {
        if (!co_await service::sync_runtime::renew_running_lease(completion, marker)) {
            throw std::runtime_error("网站同步标记 lease 已失效");
        }
        result_transition = co_await service::sync_runtime::complete_running_and_record_event(
            completion, marker, attempt.to_json(context.pool()));
        if (!result_transition.marker_transitioned_) {
            throw std::runtime_error("网站同步标记 lease 已失效");
        }
    }
    co_await service::sync_runtime::commit_and_publish_result_event(completion, context.db(), marker,
        result_transition, trace);
    save_step.complete();
    co_return;
}

}  // namespace service::website_dns
