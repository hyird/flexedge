#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/model_json.h>

#include <algorithm>
#include <cstdint>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "service/features/background/worker_pool.h"
#include "service/features/dns/driver.h"
#include "service/features/dns_sync/failure.h"
#include "service/features/dns_sync/mapper.h"
#include "service/features/dns_sync/reconciliation.h"
#include "service/features/dns_sync/task.h"
#include "service/features/dns_sync/zone_loader.h"
#include "service/features/dns_sync/zone_persistence.h"
#include "service/features/sync_runtime/state.h"
#include "service/utils/secret.h"
#include "service/utils/sensitive_string.h"

namespace service::dns_sync::detail {

inline ruvia::task<bool>
merge_remote_records(service::background::worker_context& context, const dns_task& task,
    const service::sync_runtime::running_marker_lease& lease,
    std::int64_t config_revision, std::int64_t desired_revision,
    const zone_config_data& config, const zone_runtime_data& runtime,
    const zone_config_data& desired_config, std::string_view domain,
    const service::dns::dns_provider_driver& driver,
    const std::vector<service::dns::provider_record>& remote_records,
    const std::vector<service::dns::provider_line>& lines) {
    auto plan = plan_remote_merge(service::sync_runtime::marker_operation_name(task.operation_), config,
        runtime, desired_config, domain, driver.record_names(), remote_records, lines);
    if (!plan.conflicts_.empty()) {
        co_await store_conflicts(context, task, lease, desired_revision, runtime, lines,
            plan.conflicts_);
        co_return true;
    }
    if (!plan.changed_) {
        co_return false;
    }
    co_return co_await persist_remote_merge(context, task, lease, config_revision, desired_revision,
        std::move(plan), runtime, lines);
}

struct desired_record_ids final {
    std::unordered_set<std::string> local_;
    std::unordered_set<std::string> remote_;
};

inline desired_record_ids
collect_desired_record_ids(const zone_config_data& desired_config,
    const std::unordered_map<std::string, std::string>& known_remote_ids,
    bool deleted) {
    desired_record_ids result;
    if (deleted) {
        return result;
    }
    result.local_.reserve(desired_config.records_.size());
    result.remote_.reserve(desired_config.records_.size());
    for (const auto& record : desired_config.records_) {
        result.local_.emplace(record.id_);
        if (const auto known = known_remote_ids.find(record.id_); known != known_remote_ids.end()) {
            result.remote_.emplace(known->second);
        }
    }
    return result;
}

inline ruvia::task<void> delete_obsolete_remote_records(
    service::background::worker_context& context,
    const service::sync_runtime::running_marker_lease& lease,
    const service::dns::dns_provider_driver& driver, std::string_view account_id,
    std::string_view secret, std::string_view remote_zone_id,
    const std::unordered_map<std::string, std::string>& known_remote_ids,
    const desired_record_ids& desired, std::vector<service::dns::provider_record>& remote_records) {
    for (const auto& [record_id, remote_id] : known_remote_ids) {
        if (desired.local_.contains(record_id) || desired.remote_.contains(remote_id) ||
            remote_id.empty()) {
            continue;
        }
        const auto found = std::ranges::find_if(
            remote_records, [&](const auto& record) { return record.id_ == remote_id; });
        if (found == remote_records.end()) {
            continue;
        }
        auto db = context.db();
        if (!co_await service::sync_runtime::renew_running_lease(db, lease)) {
            throw std::runtime_error("DNS 同步标记 lease 已失效");
        }
        co_await driver.delete_record(context, account_id, secret, remote_zone_id, remote_id);
        std::erase_if(remote_records, [&](const auto& record) { return record.id_ == remote_id; });
    }
    co_return;
}

inline ruvia::task<zone_runtime_dto> reconcile_zone_records(
    service::background::worker_context& context,
    const service::sync_runtime::running_marker_lease& lease,
    const service::dns::dns_provider_driver& driver, std::string_view account_id,
    std::string_view secret, std::string_view remote_zone_id, std::string_view domain,
    const zone_config_data& desired_config, const zone_runtime_data& current_runtime,
    const std::unordered_map<std::string, std::string>& known_remote_ids,
    std::vector<service::dns::provider_record>& remote_records,
    const std::vector<service::dns::provider_line>& lines, std::int64_t revision, bool deleted) {
    zone_runtime_dto next_runtime({.resource_ = context.pool()});
    next_runtime.set<"records_imported">(true);
    auto& runtime_lines = next_runtime.ensure<"lines">();
    for (const auto& line : lines) {
        auto& output_line =
            runtime_lines.emplace_back(ruvia::model_options{.resource_ = context.pool()});
        output_line.set<"code">(line.code_);
        output_line.set<"name">(line.name_);
        output_line.set<"display_name">(line.display_name_);
        output_line.set<"status">("enabled");
    }
    auto& states = next_runtime.ensure<"record_states">();
    (void)next_runtime.ensure<"conflicts">();
    if (!deleted) {
        for (const auto& record : desired_config.records_) {
            auto db = context.db();
            if (!co_await service::sync_runtime::renew_running_lease(db, lease)) {
                throw std::runtime_error("DNS 同步标记 lease 已失效");
            }
            const auto managed = to_managed_record(record);
            const auto known = known_remote_ids.find(managed.id_);
            const auto remote_id = co_await driver.reconcile_record(
                context, account_id, secret, remote_zone_id, domain,
                known == known_remote_ids.end() ? std::string_view{}
                                                : std::string_view{known->second},
                managed.type_, managed.name_, managed.content_, managed.ttl_, managed.priority_,
                managed.proxied_, managed.line_code_, remote_records);
            auto& state = states.emplace_back(ruvia::model_options{.resource_ = context.pool()});
            state.set<"id">(managed.id_);
            state.set<"remote_record_id">(remote_id);
            state.set<"sync_status">("synced");
            state.set<"synced_revision">(revision);

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
    append_challenge_records(next_runtime, current_runtime);
    co_return next_runtime;
}

inline ruvia::task<std::int64_t> sync_zone(service::background::worker_context& context,
    const dns_task& task) {
    const auto lease = service::sync_runtime::make_running_lease(
        task.tenant_id_, task.id_, task.version_, context.lease_owner(),
        service::sync_runtime::marker_resource_type::dns_zone, task.resource_id_);
    const auto state = co_await load_zone_sync_state(context, task, lease);
    if (!state) {
        co_return task.version_;
    }
    service::utils::sensitive_string secret(
        service::utils::open_secret(state->provider_config_.credential_envelope_));
    const service::dns::dns_provider_driver driver(state->provider_);
    for (const auto& record : state->desired_.records_) {
        if (record.ttl_ < driver.minimum_record_ttl()) {
            throw std::runtime_error("DNS 配置中的 TTL 低于当前服务商允许的最小值");
        }
    }

    remote_zone_data remote;
    bool remote_zone_missing = false;
    try {
        remote = co_await load_remote_zone_data(context, lease, driver, state->account_id_,
            secret.view(), state->domain_);
    } catch (const service::dns::cloudflare_error& error) {
        if (task.operation_ == service::sync_runtime::marker_operation::remove &&
            error.code() == service::dns::cloudflare_error_code::zone_not_found) {
            remote_zone_missing = true;
        } else {
            throw;
        }
    } catch (const service::dns::aliyun_error& error) {
        if (task.operation_ == service::sync_runtime::marker_operation::remove &&
            error.code() == service::dns::aliyun_error_code::domain_not_found) {
            remote_zone_missing = true;
        } else {
            throw;
        }
    }
    if (remote_zone_missing) {
        co_await finish_deleted_zone_without_remote(context, task, lease);
        co_return task.version_;
    }
    const bool imported = state->runtime_.records_imported_;
    if (!state->deleted_ && !imported && state->config_.records_.empty()) {
        co_await import_initial_remote_records(context, task, lease, state->config_revision_,
            state->domain_, state->runtime_, driver, remote.records_,
            remote.lines_);
        co_return task.version_;
    }
    if (!state->deleted_ &&
        co_await merge_remote_records(context, task, lease, state->config_revision_, state->revision_,
            state->config_, state->runtime_, state->desired_, state->domain_,
            driver, remote.records_, remote.lines_)) {
        co_return task.version_;
    }

    const auto known_remote_ids = collect_remote_ids_by_local_id(state->runtime_);
    const auto desired = collect_desired_record_ids(state->desired_, known_remote_ids, state->deleted_);
    co_await delete_obsolete_remote_records(context, lease, driver, state->account_id_, secret.view(),
        remote.id_, known_remote_ids, desired, remote.records_);
    const auto next_runtime = co_await reconcile_zone_records(
        context, lease, driver, state->account_id_, secret.view(), remote.id_, state->domain_,
        state->desired_, state->runtime_, known_remote_ids, remote.records_, remote.lines_,
        state->revision_, state->deleted_);
    co_await persist_synced_zone(context, task, lease, next_runtime, state->revision_);
    co_return state->revision_;
}

}  // namespace service::dns_sync::detail
