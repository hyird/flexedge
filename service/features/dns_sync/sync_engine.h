#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/model_json.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "service/features/background/task_context.h"
#include "service/features/dns/driver.h"
#include "service/features/dns_sync/attempt_detail.h"
#include "service/features/dns_sync/mapper.h"
#include "service/features/dns_sync/reconciliation.h"
#include "service/features/dns_sync/task.h"
#include "service/features/dns_sync/zone_loader.h"
#include "service/features/dns_sync/zone_persistence.h"
#include "service/features/sync_runtime/state.h"
#include "service/utils/secret.h"
#include "service/utils/sensitive_string.h"

namespace service::dns_sync::detail {

inline ruvia::task<void> delete_obsolete_remote_records(
    service::background::task_context& context, service::attempt_trace::recorder& trace,
    const service::sync_runtime::running_marker_lease& lease,
    const service::dns::dns_provider_driver& driver, std::string_view account_id,
    std::string_view secret, std::string_view remote_zone_id, std::string_view domain,
    const std::unordered_set<std::string>& obsolete, std::vector<service::dns::provider_record>& remote_records,
    zone_sync_attempt& attempt) {
    auto step = trace.begin_step(service::attempt_trace::step_code::delete_obsolete, domain);
    std::size_t deleted_count = 0;
    for (const auto& remote_id : obsolete) {
        const auto found = std::ranges::find_if(
            remote_records, [&](const auto& record) { return record.id_ == remote_id; });
        if (found == remote_records.end()) {
            continue;
        }
        auto deleted = to_attempt_record(*found, driver.record_names(), domain);
        auto db = context.db();
        if (!co_await service::sync_runtime::renew_running_lease(db, lease)) {
            throw std::runtime_error("DNS 同步标记 lease 已失效");
        }
        const auto subject = deleted.name_ + " " + deleted.type_;
        co_await driver.delete_record(context, trace, account_id, secret, remote_zone_id, remote_id, subject);
        attempt.record_deleted(std::move(deleted));
        ++deleted_count;
        std::erase_if(remote_records, [&](const auto& record) { return record.id_ == remote_id; });
    }
    if (deleted_count == 0) {
        step.skip("没有需要删除的远程记录");
    } else {
        trace.log(service::attempt_trace::log_level::info, "已删除 " + std::to_string(deleted_count) + " 条远程记录");
        step.complete();
    }
    co_return;
}

// The runtime an attempt persists and the desired records it left unpushed.
struct reconciled_zone final {
    std::vector<zone_record_runtime_data> states_;
    std::vector<unsynced_record> unsynced_;
};

[[nodiscard]] inline std::int64_t previous_synced_revision(const zone_runtime_data& runtime,
    std::string_view id) {
    const auto state = std::ranges::find_if(
        runtime.record_states_, [id](const auto& item) { return item.id_ && *item.id_ == id; });
    return state == runtime.record_states_.end() ? 0 : state->synced_revision_.value_or(0);
}

inline ruvia::task<reconciled_zone> reconcile_zone_records(
    service::background::task_context& context, service::attempt_trace::recorder& trace,
    const service::sync_runtime::running_marker_lease& lease,
    const service::dns::dns_provider_driver& driver, std::string_view account_id,
    std::string_view secret, std::string_view remote_zone_id, std::string_view domain,
    const zone_config_data& desired_config, const zone_runtime_data& current_runtime,
    const std::unordered_map<std::string, std::string>& known_remote_ids,
    const std::vector<unsynced_record>& blocked,
    std::vector<service::dns::provider_record>& remote_records,
    std::int64_t revision, bool deleted,
    zone_sync_attempt& attempt) {
    auto step = trace.begin_step(service::attempt_trace::step_code::reconcile_records, domain);
    std::size_t unchanged = 0;
    reconciled_zone result;
    if (!deleted) {
        for (const auto& record : desired_config.records_) {
            auto db = context.db();
            if (!co_await service::sync_runtime::renew_running_lease(db, lease)) {
                throw std::runtime_error("DNS 同步标记 lease 已失效");
            }
            const auto& managed = record;
            const auto known = known_remote_ids.find(managed.id_);
            std::string unsynced_error;
            std::optional<service::dns::reconciled_record> reconciled;
            if (const auto held = std::ranges::find(blocked, managed.id_, &unsynced_record::id_);
                held != blocked.end()) {
                unsynced_error = held->error_;
            } else {
                // The provider is the final judge of which records share a name; its rejection
                // fails this record only, so the rest of the zone still syncs.
                try {
                    reconciled = co_await driver.reconcile_record(
                        context, trace, account_id, secret, remote_zone_id, domain,
                        known == known_remote_ids.end() ? std::string_view{}
                                                        : std::string_view{known->second},
                        managed.type_, managed.name_, managed.content_, managed.ttl_, managed.priority_,
                        managed.proxied_, managed.line_code_, remote_records);
                } catch (const service::dns::provider_error& error) {
                    if (error.code() != service::dns::provider_error_code::record_name_conflict) {
                        throw;
                    }
                    unsynced_error = error.what();
                }
            }
            if (!reconciled) {
                const auto names = driver.record_names();
                auto name = names.local_record_name(names.remote_record_name(managed.name_, domain), domain);
                trace.log(service::attempt_trace::log_level::warning,
                    "记录 " + name + " " + managed.type_ + " 未同步：" + unsynced_error);
                result.states_.push_back({.id_ = managed.id_,
                    .remote_record_id_ = known == known_remote_ids.end() ? std::nullopt : std::optional{known->second},
                    .sync_status_ = "failed",
                    .synced_revision_ = previous_synced_revision(current_runtime, managed.id_),
                    .last_error_ = unsynced_error,
                    .managed_ = true});
                result.unsynced_.push_back({.id_ = managed.id_,
                    .name_ = std::move(name),
                    .type_ = managed.type_,
                    .error_ = std::move(unsynced_error)});
                continue;
            }
            const auto& remote_id = reconciled->remote_id_;
            if (reconciled->action_ == service::dns::record_reconciliation_action::reuse) {
                attempt.record_unchanged();
                ++unchanged;
            } else if (reconciled->action_ == service::dns::record_reconciliation_action::update &&
                       reconciled->previous_) {
                attempt.record_updated(to_attempt_record(*reconciled->previous_, driver.record_names(), domain),
                    to_attempt_record(managed));
            } else {
                attempt.record_created(to_attempt_record(managed));
            }
            result.states_.push_back({.id_ = managed.id_, .remote_record_id_ = remote_id, .sync_status_ = "synced", .synced_revision_ = revision, .last_error_ = {}, .managed_ = true});

            auto resolved = to_remote_record(managed, driver.record_names(), domain);
            resolved.id_ = remote_id;
            const auto existing = std::ranges::find_if(
                remote_records, [&](const auto& item) { return item.id_ == remote_id; });
            if (existing == remote_records.end()) {
                remote_records.push_back(std::move(resolved));
            } else {
                *existing = std::move(resolved);
            }
        }
    }
    if (unchanged > 0) {
        trace.log(service::attempt_trace::log_level::info,
            std::to_string(unchanged) + " 条系统记录与服务商一致，无需写入");
    }
    step.complete();
    co_return result;
}

inline ruvia::task<std::int64_t> sync_zone(service::background::task_context& context,
    service::attempt_trace::recorder& trace, const dns_task& task, zone_sync_attempt& attempt) {
    using service::attempt_trace::step_code;
    const auto lease = service::sync_runtime::make_running_lease(
        task.tenant_id_, task.id_, task.version_, context.lease_owner(), task.claimed_at_,
        service::sync_runtime::marker_resource_type::dns_zone, task.resource_id_);
    auto load_step = trace.begin_step(step_code::load_state);
    const auto state = co_await load_zone_sync_state(context, task, lease);
    if (!state) {
        load_step.skip("配置已更新，本次让位于新的同步");
        co_return task.version_;
    }
    trace.log(service::attempt_trace::log_level::info,
        "托管域 " + state->domain_ + "，服务商 " + state->provider_ + "，期望记录 " +
            std::to_string(state->desired_.records_.size()) + " 条");
    load_step.complete();
    auto credential_step = trace.begin_step(step_code::open_credential);
    service::utils::sensitive_string secret(
        service::utils::open_secret(state->provider_config_.credential_envelope_));
    trace.register_secret(secret.view());
    trace.register_secret(state->account_id_);
    credential_step.complete();
    const service::dns::dns_provider_driver driver(state->provider_,
        {.tenant_id_ = task.tenant_id_, .account_id_ = state->account_id_});
    auto ttl_step = trace.begin_step(step_code::validate_ttl);
    for (const auto& record : state->desired_.records_) {
        if (record.ttl_ < driver.minimum_record_ttl()) {
            ttl_step.fail("记录 " + record.name_ + " " + record.type_ + " 的 TTL " + std::to_string(record.ttl_) +
                          " 低于服务商允许的最小值 " + std::to_string(driver.minimum_record_ttl()));
            throw std::runtime_error("DNS 配置中的 TTL 低于当前服务商允许的最小值");
        }
    }
    ttl_step.complete();

    remote_zone_data remote;
    bool remote_zone_missing = false;
    try {
        remote = co_await load_remote_zone_data(context, trace, lease, driver, state->account_id_,
            secret.view(), state->domain_);
    } catch (const service::dns::provider_error& error) {
        if (task.operation_ != service::sync_runtime::marker_operation::remove ||
            error.code() != service::dns::provider_error_code::zone_not_found) {
            throw;
        }
        remote_zone_missing = true;
    }
    if (remote_zone_missing) {
        co_await finish_deleted_zone_without_remote(context, task, lease);
        co_return task.version_;
    }
    auto known_remote_ids = collect_remote_ids_by_local_id(state->runtime_);
    for (const auto& record : state->desired_.records_) {
        if (record.type_ != "CNAME" || known_remote_ids.contains(record.id_)) {
            continue;
        }
        const service::dns::provider_record* candidate = nullptr;
        for (const auto& remote_record : remote.records_) {
            if (remote_record.type_ != "CNAME" || remote_record.line_code_ != record.line_code_ ||
                service::common::normalize_domain_name(driver.record_names().remote_record_name(remote_record.name_, state->domain_)) !=
                    service::common::normalize_domain_name(driver.record_names().remote_record_name(record.name_, state->domain_))) {
                continue;
            }
            if (candidate) {
                candidate = nullptr;
                break;
            }
            candidate = &remote_record;
        }
        if (candidate) {
            known_remote_ids.emplace(record.id_, candidate->id_);
        }
    }
    const auto obsolete = obsolete_system_record_ids(state->runtime_,
        state->deleted_ ? zone_config_data{} : state->desired_, remote.records_, state->cluster_hostnames_,
        driver.record_names(), state->domain_);
    co_await delete_obsolete_remote_records(context, trace, lease, driver, state->account_id_, secret.view(),
        remote.id_, state->domain_, obsolete, remote.records_, attempt);
    std::unordered_set<std::string> managed_remote_ids;
    for (const auto& record : state->desired_.records_) {
        const auto known = known_remote_ids.find(record.id_);
        if (known != known_remote_ids.end()) {
            managed_remote_ids.emplace(known->second);
        }
    }
    auto observed = state->desired_;
    for (const auto& record : remote.records_) {
        if (managed_remote_ids.contains(record.id_)) {
            continue;
        }
        observed.records_.push_back({.id_ = record.id_, .type_ = record.type_, .name_ = record.name_, .content_ = record.content_, .ttl_ = record.ttl_, .priority_ = record.priority_, .proxied_ = record.proxied_, .line_code_ = record.line_code_});
    }
    const auto blocked = find_blocked_website_records(observed, state->website_record_ids_,
        driver.record_names(), state->domain_);
    const auto reconciled = co_await reconcile_zone_records(
        context, trace, lease, driver, state->account_id_, secret.view(), remote.id_, state->domain_,
        state->desired_, state->runtime_, known_remote_ids, blocked, remote.records_,
        state->revision_, state->deleted_, attempt);
    auto persist_step = trace.begin_step(step_code::persist_result, state->domain_);
    const auto snapshot = capture_zone_snapshot(task.resource_id_, state->domain_, driver.record_names(),
        remote.records_, remote.lines_, state->runtime_, reconciled.states_, state->revision_);
    co_await persist_synced_zone(context, trace, task, lease, snapshot, state->revision_, state->snapshot_revision_,
        unsynced_records_error(reconciled.unsynced_), attempt.to_json(context.pool()), attempt.changed());
    persist_step.complete();
    co_return state->revision_;
}

}  // namespace service::dns_sync::detail
