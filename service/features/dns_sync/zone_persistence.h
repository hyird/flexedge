#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/model_json.h>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "service/features/background/worker_pool.h"
#include "service/features/dns/driver.h"
#include "service/features/dns_sync/mapper.h"
#include "service/features/dns_sync/queue.h"
#include "service/features/dns_sync/reconciliation.h"
#include "service/features/dns_sync/task.h"
#include "service/features/sync_runtime/error.h"
#include "service/features/sync_runtime/state.h"

namespace service::dns_sync::detail {

inline ruvia::task<void>
import_initial_remote_records(service::background::worker_context& context, const dns_task& task,
    const service::sync_runtime::running_marker_lease& lease,
    std::int64_t config_revision, std::string_view domain,
    const zone_runtime_data& current_runtime,
    const service::dns::dns_provider_driver& driver,
    const std::vector<service::dns::provider_record>& remote_records,
    const std::vector<service::dns::provider_line>& lines) {
    if (remote_records.size() > max_reconciliation_records) {
        throw std::runtime_error("远程 DNS 记录超过单个域名可导入上限");
    }

    std::unordered_set<std::string> enabled_lines;
    enabled_lines.reserve(lines.size());
    for (const auto& line : lines) {
        if (!line.code_.empty()) {
            enabled_lines.emplace(line.code_);
        }
    }

    std::unordered_set<std::string> remote_ids;
    remote_ids.reserve(remote_records.size());
    for (const auto& record : remote_records) {
        if (!supports_managed_record_type(record.type_)) {
            throw std::runtime_error("远程 DNS 存在当前不支持自动托管的记录类型：" + record.type_);
        }
        if (record.id_.empty() || record.name_.empty() || record.content_.empty() || record.ttl_ < 1 ||
            record.ttl_ > 86400 ||
            (record.priority_ && (*record.priority_ < 0 || *record.priority_ > 65535)) ||
            !enabled_lines.contains(record.line_code_)) {
            throw std::runtime_error("远程 DNS 记录不符合本地托管配置要求");
        }
        if (!remote_ids.emplace(record.id_).second) {
            throw std::runtime_error("远程 DNS 记录 ID 重复");
        }
    }

    auto transaction = co_await context.db().begin_transaction();
    const auto ids = co_await transaction.query(
        "SELECT gen_random_uuid()::TEXT FROM generate_series(1, $1::BIGINT)",
        static_cast<std::int64_t>(remote_records.size()));
    if (ids.size() != remote_records.size()) {
        throw std::runtime_error("远程 DNS 记录本地 ID 生成失败");
    }

    zone_config_output imported_config({.resource_ = context.pool()});
    auto& config_records = imported_config.ensure<"records">();
    zone_runtime_dto imported_runtime({.resource_ = context.pool()});
    imported_runtime.set<"records_imported">(true);
    auto& runtime_lines = imported_runtime.ensure<"lines">();
    for (const auto& line : lines) {
        auto& output_line =
            runtime_lines.emplace_back(ruvia::model_options{.resource_ = context.pool()});
        output_line.set<"code">(line.code_);
        output_line.set<"name">(line.name_);
        output_line.set<"display_name">(line.display_name_);
        output_line.set<"status">("enabled");
    }
    auto& states = imported_runtime.ensure<"record_states">();
    (void)imported_runtime.ensure<"conflicts">();
    append_challenge_records(imported_runtime, current_runtime);
    for (std::size_t index = 0; index < remote_records.size(); ++index) {
        const auto& remote = remote_records[index];
        const auto id = std::string(ids[index][0].value().value_or(""));
        if (id.empty()) {
            throw std::runtime_error("远程 DNS 记录本地 ID 为空");
        }
        auto& record =
            config_records.emplace_back(ruvia::model_options{.resource_ = context.pool()});
        record.set<"id">(id);
        record.set<"type">(remote.type_);
        record.set<"name">(driver.record_names().local_record_name(remote.name_, domain));
        record.set<"content">(remote.content_);
        record.set<"ttl">(remote.ttl_);
        record.set<"proxied">(remote.proxied_);
        record.set<"line_code">(remote.line_code_);
        if (remote.priority_) {
            record.set<"priority">(*remote.priority_);
        }
        auto& state = states.emplace_back(ruvia::model_options{.resource_ = context.pool()});
        state.set<"id">(id);
        state.set<"remote_record_id">(remote.id_);
        state.set<"sync_status">("pending");
        state.set<"synced_revision">(0);
    }

    const auto config_json = ruvia::to_json(imported_config, {.resource_ = context.pool()});
    const auto runtime_json = ruvia::to_json(imported_runtime, {.resource_ = context.pool()});
    const auto updated = co_await transaction.query(
        "UPDATE sys_dns_zone SET config = $1::jsonb, runtime = $2::jsonb, revision = revision + "
        "1, desired_revision = desired_revision + 1, sync_status = 'pending', last_error = NULL, "
        "updated_at = NOW() WHERE id = $3 AND tenant_id = $4 AND revision = $5 AND "
        "desired_revision = $6 AND deleted_at IS NULL RETURNING desired_revision",
        std::string_view(config_json), std::string_view(runtime_json), task.resource_id_, task.tenant_id_,
        config_revision, task.version_);
    if (updated.empty()) {
        (void)co_await service::sync_runtime::release_running(transaction, lease);
        co_await transaction.commit();
        co_return;
    }
    if (!co_await service::sync_runtime::release_running(transaction, lease)) {
        throw std::runtime_error("DNS 同步标记 lease 已失效");
    }
    (void)co_await enqueue_zone_revision(
        transaction, task.tenant_id_, task.resource_id_,
        updated.front()[0].as<std::int64_t>().value_or(task.version_ + 1));
    co_await transaction.commit();
    co_return;
}

inline ruvia::task<void>
store_conflicts(service::background::worker_context& context, service::attempt_trace::recorder& trace,
    const dns_task& task,
    const service::sync_runtime::running_marker_lease& lease, std::int64_t desired_revision,
    const zone_runtime_data& runtime, const std::vector<service::dns::provider_line>& lines,
    const std::vector<record_conflict>& conflicts, std::string_view detail) {
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
    for (const auto& state : runtime.record_states_) {
        if (!state.id_) {
            continue;
        }
        auto& output = states.emplace_back(ruvia::model_options{.resource_ = context.pool()});
        output.set<"id">(*state.id_);
        output.set<"sync_status">(state.sync_status_ ? *state.sync_status_
                                                     : std::string_view{"pending"});
        output.set<"synced_revision">(state.synced_revision_ ? *state.synced_revision_ : 0);
        if (state.remote_record_id_) {
            output.set<"remote_record_id">(*state.remote_record_id_);
        }
        if (state.last_error_) {
            output.set<"last_error">(*state.last_error_);
        }
    }
    auto& output_conflicts = next_runtime.ensure<"conflicts">();
    for (const auto& conflict : conflicts) {
        auto& output_conflict =
            output_conflicts.emplace_back(ruvia::model_options{.resource_ = context.pool()});
        output_conflict.set<"id">(conflict.id_);
        output_conflict.set<"type">(conflict.type_);
        output_conflict.set<"name">(conflict.name_);
        output_conflict.set<"line_code">(conflict.line_code_);
        output_conflict.set<"local_content">(conflict.local_content_);
        output_conflict.set<"remote_content">(conflict.remote_content_);
    }
    append_challenge_records(next_runtime, runtime);

    const auto runtime_json = ruvia::to_json(next_runtime, {.resource_ = context.pool()});
    auto transaction = co_await context.db().begin_transaction();
    const auto updated = co_await transaction.execute(
        "UPDATE sys_dns_zone SET runtime = $1::jsonb, sync_status = 'conflict', last_error = "
        "NULL, updated_at = NOW() WHERE id = $2 AND tenant_id = $3 AND desired_revision = "
        "$4 AND deleted_at IS NULL",
        std::string_view(runtime_json), task.resource_id_, task.tenant_id_, desired_revision);
    service::sync_runtime::running_result_transition result_transition;
    if (updated.affected_rows() != 0) {
        if (!co_await service::sync_runtime::renew_running_lease(transaction, lease)) {
            throw std::runtime_error("DNS 同步标记 lease 已失效");
        }
        result_transition = co_await service::sync_runtime::complete_running_and_record_event(
            transaction, lease, detail);
        if (!result_transition.marker_transitioned_) {
            throw std::runtime_error("DNS 同步标记 lease 已失效");
        }
    } else {
        (void)co_await service::sync_runtime::release_running(transaction, lease);
    }
    co_await service::sync_runtime::commit_and_publish_result_event(transaction, context.db(), lease,
        result_transition, trace);
    co_return;
}

inline ruvia::task<bool> persist_remote_merge(service::background::worker_context& context,
    const dns_task& task,
    const service::sync_runtime::running_marker_lease& lease,
    std::int64_t config_revision,
    std::int64_t desired_revision, remote_merge_plan plan,
    const zone_runtime_data& runtime,
    const std::vector<service::dns::provider_line>& lines) {
    auto& merged = plan.records_;
    auto& remote_ids_by_local_id = plan.remote_ids_by_local_id_;
    auto transaction = co_await context.db().begin_transaction();
    const auto new_id_count = static_cast<std::int64_t>(std::count_if(
        merged.begin(), merged.end(), [](const auto& record) { return record.id_.empty(); }));
    const auto ids = co_await transaction.query(
        "SELECT gen_random_uuid()::TEXT FROM generate_series(1, $1::BIGINT)", new_id_count);
    if (ids.size() != static_cast<std::size_t>(new_id_count)) {
        throw std::runtime_error("远程 DNS 记录本地 ID 生成失败");
    }
    std::size_t next_id = 0;
    for (auto& record : merged) {
        if (!record.id_.empty()) {
            continue;
        }
        record.id_ = std::string(ids[next_id++][0].value().value_or(""));
        if (record.id_.empty()) {
            throw std::runtime_error("远程 DNS 记录本地 ID 为空");
        }
        if (record.remote_id_.empty()) {
            throw std::runtime_error("远程 DNS 记录合并状态损坏");
        }
        remote_ids_by_local_id.emplace(record.id_, record.remote_id_);
    }

    zone_config_output merged_config({.resource_ = context.pool()});
    auto& config_records = merged_config.ensure<"records">();
    for (const auto& record : merged) {
        auto& output =
            config_records.emplace_back(ruvia::model_options{.resource_ = context.pool()});
        output.set<"id">(record.id_);
        output.set<"type">(record.type_);
        output.set<"name">(record.name_);
        output.set<"content">(record.content_);
        output.set<"ttl">(record.ttl_);
        output.set<"proxied">(record.proxied_);
        output.set<"line_code">(record.line_code_);
        if (record.priority_) {
            output.set<"priority">(*record.priority_);
        }
    }

    zone_runtime_dto merged_runtime({.resource_ = context.pool()});
    merged_runtime.set<"records_imported">(true);
    auto& runtime_lines = merged_runtime.ensure<"lines">();
    for (const auto& line : lines) {
        auto& output_line =
            runtime_lines.emplace_back(ruvia::model_options{.resource_ = context.pool()});
        output_line.set<"code">(line.code_);
        output_line.set<"name">(line.name_);
        output_line.set<"display_name">(line.display_name_);
        output_line.set<"status">("enabled");
    }
    auto& states = merged_runtime.ensure<"record_states">();
    (void)merged_runtime.ensure<"conflicts">();
    for (const auto& [id, remote_id] : remote_ids_by_local_id) {
        auto& state = states.emplace_back(ruvia::model_options{.resource_ = context.pool()});
        state.set<"id">(id);
        state.set<"remote_record_id">(remote_id);
        state.set<"sync_status">("pending");
        state.set<"synced_revision">(0);
    }
    append_challenge_records(merged_runtime, runtime);

    const auto config_json = ruvia::to_json(merged_config, {.resource_ = context.pool()});
    const auto runtime_json = ruvia::to_json(merged_runtime, {.resource_ = context.pool()});
    const auto updated = co_await transaction.query(
        "UPDATE sys_dns_zone SET config = $1::jsonb, runtime = $2::jsonb, revision = revision + "
        "1, desired_revision = desired_revision + 1, sync_status = 'pending', last_error = NULL, "
        "updated_at = NOW() WHERE id = $3 AND tenant_id = $4 AND revision = $5 AND "
        "desired_revision = $6 AND deleted_at IS NULL RETURNING desired_revision",
        std::string_view(config_json), std::string_view(runtime_json), task.resource_id_, task.tenant_id_,
        config_revision, desired_revision);
    if (updated.empty()) {
        (void)co_await service::sync_runtime::release_running(transaction, lease);
        co_await transaction.commit();
        co_return false;
    }
    if (!co_await service::sync_runtime::renew_running_lease(transaction, lease)) {
        throw std::runtime_error("DNS 同步标记 lease 已失效");
    }
    (void)co_await enqueue_zone_revision(
        transaction, task.tenant_id_, task.resource_id_,
        updated.front()[0].as<std::int64_t>().value_or(desired_revision + 1), task.operation_);
    (void)co_await service::sync_runtime::release_running(transaction, lease);
    co_await transaction.commit();
    co_return true;
}

inline ruvia::task<void> persist_synced_zone(service::background::worker_context& context,
    service::attempt_trace::recorder& trace, const dns_task& task,
    const service::sync_runtime::running_marker_lease& lease,
    const zone_runtime_dto& runtime, std::int64_t revision, std::string_view detail) {
    const auto runtime_json = ruvia::to_json(runtime, {.resource_ = context.pool()});
    auto transaction = co_await context.db().begin_transaction();
    const auto updated = co_await transaction.execute(
        "UPDATE sys_dns_zone SET runtime = $2::jsonb, synced_revision = GREATEST("
        "synced_revision, $3), sync_status = CASE WHEN desired_revision <= $3 THEN 'synced' ELSE "
        "'pending' END, last_synced_at = NOW(), last_error = NULL, updated_at = NOW() WHERE id = "
        "$1 AND tenant_id = $4 AND desired_revision = $3",
        task.resource_id_, std::string_view(runtime_json), revision, task.tenant_id_);
    service::sync_runtime::running_result_transition result_transition;
    if (updated.affected_rows() == 0) {
        (void)co_await service::sync_runtime::release_running(transaction, lease);
    } else {
        if (!co_await service::sync_runtime::renew_running_lease(transaction, lease)) {
            throw std::runtime_error("DNS 同步标记 lease 已失效");
        }
        if (task.operation_ == service::sync_runtime::marker_operation::remove) {
            if (!co_await service::sync_runtime::remove_running(transaction, lease)) {
                throw std::runtime_error("DNS 删除同步标记 lease 已失效");
            }
        } else {
            result_transition = co_await service::sync_runtime::complete_running_and_record_event(
                transaction, lease, detail);
            if (!result_transition.marker_transitioned_) {
                throw std::runtime_error("DNS 同步标记 lease 已失效");
            }
        }
    }
    co_await service::sync_runtime::commit_and_publish_result_event(transaction, context.db(), lease,
        result_transition, trace);
    co_return;
}

inline ruvia::task<void>
finish_deleted_zone_without_remote(service::background::worker_context& context, const dns_task& task,
    const service::sync_runtime::running_marker_lease& lease) {
    auto transaction = co_await context.db().begin_transaction();
    const auto updated = co_await transaction.execute(
        "UPDATE sys_dns_zone SET synced_revision = GREATEST(synced_revision, $3), "
        "sync_status = 'synced', last_synced_at = NOW(), last_error = NULL, updated_at = NOW() "
        "WHERE id = $1 AND tenant_id = $2 AND desired_revision = $3",
        task.resource_id_, task.tenant_id_, task.version_);
    if (updated.affected_rows() != 0 &&
        !co_await service::sync_runtime::remove_running(transaction, lease)) {
        throw std::runtime_error("DNS 删除同步标记 lease 已失效");
    }
    if (updated.affected_rows() == 0) {
        (void)co_await service::sync_runtime::release_running(transaction, lease);
    }
    co_await transaction.commit();
    co_return;
}

inline ruvia::task<void> fail_dns_task(service::background::worker_context& context,
    service::attempt_trace::recorder& trace, const dns_task& task, std::string_view error, bool permanent,
    std::string_view detail) {
    const auto message = service::sync_runtime::attempt_error(trace, error);
    const auto lease = service::sync_runtime::make_running_lease(
        task.tenant_id_, task.id_, task.version_, context.lease_owner(), task.claimed_at_,
        service::sync_runtime::marker_resource_type::dns_zone, task.resource_id_);
    auto transaction = co_await context.db().begin_transaction();
    (void)co_await transaction.query(
        "SELECT id FROM sys_dns_zone WHERE tenant_id = $1 AND id = $2 LIMIT 1 FOR UPDATE",
        task.tenant_id_, task.resource_id_);
    const auto result_transition = co_await service::sync_runtime::fail_running_and_record_event(
        transaction, lease, message, detail);
    if (result_transition.marker_transitioned_) {
        (void)co_await transaction.execute(
            "UPDATE sys_dns_zone SET sync_status = $2, last_error = $3, updated_at = NOW() WHERE "
            "id = $1 AND desired_revision = $4 AND tenant_id = $5",
            task.resource_id_, permanent ? std::string_view{"failed"} : std::string_view{"pending"},
            std::string_view(message), task.version_, task.tenant_id_);
    }
    co_await service::sync_runtime::commit_and_publish_result_event(transaction, context.db(), lease,
        result_transition, trace);
    if (result_transition.marker_transitioned_) {
        service::sync_runtime::log_attempt_failure("DNS sync task", task.id_, message);
    }
    co_return;
}

}  // namespace service::dns_sync::detail
