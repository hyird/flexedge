#pragma once

#include <ruvia/core/task.h>
#include <ruvia/core/timer.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "service/features/background/worker_pool.h"
#include "service/features/dns_sync/mapper.h"
#include "service/features/dns_sync/queue.h"
#include "service/features/sync_runtime/state.h"

namespace service::certificate_issuance {

struct dns_challenge_config_view final {
    std::string_view dns_zone_id_;
    std::string_view zone_domain_;
    std::string_view certificate_id_;
    std::int64_t minimum_record_ttl_;
    // The issuance subtask; challenge publishing and cleanup are its children.
    service::operation::work_owner owner_{};
};

// A DNS sync the issuance waits for, and the subtask doing it (empty for
// unowned issuance work).
struct queued_dns_sync final {
    std::string marker_id_;
    std::string subtask_id_;
};

struct dns_challenge_spec final {
    std::string name_;
    std::string content_;
};

inline ruvia::task<void> retire_dns_challenges_for_deleted_certificate(ruvia::db_transaction& transaction,
    std::string_view tenant_id,
    std::string_view dns_zone_id,
    std::string_view certificate_id, service::operation::work_origin& origin) {
    const auto rows = co_await transaction.query(
        "SELECT runtime::text, desired_revision FROM sys_dns_zone WHERE tenant_id = $1 AND id = "
        "$2 AND deleted_at IS NULL LIMIT 1 FOR UPDATE",
        tenant_id, dns_zone_id);
    if (rows.empty()) {
        co_return;
    }
    const auto runtime =
        service::dns_sync::parse_stored_runtime(rows.front()[0].value().value_or("{}"));
    if (!runtime) {
        throw std::runtime_error("DNS runtime 损坏");
    }
    auto next_runtime = *runtime;
    const auto old_size = next_runtime.challenge_records_.size();
    std::erase_if(next_runtime.challenge_records_,
        [&](const auto& challenge) { return challenge.certificate_id_ == certificate_id; });
    if (old_size == next_runtime.challenge_records_.size()) {
        co_return;
    }
    const auto runtime_json = ruvia::to_json(service::dns_sync::to_output(next_runtime));
    const auto updated = co_await transaction.query(
        "UPDATE sys_dns_zone SET runtime = $1::jsonb, desired_revision = desired_revision + 1, "
        "sync_status = 'pending', last_error = NULL, updated_at = NOW() WHERE tenant_id = $2 "
        "AND id = $3 AND desired_revision = $4 AND deleted_at IS NULL RETURNING desired_revision",
        std::string_view(runtime_json), tenant_id, dns_zone_id,
        rows.front()[1].as<std::int64_t>().value_or(0));
    if (updated.empty()) {
        throw std::runtime_error("DNS-01 托管域名版本已变化");
    }
    (void)co_await service::dns_sync::enqueue_zone_revision(
        transaction, std::string(tenant_id), std::string(dns_zone_id),
        updated.front()[0].as<std::int64_t>().value_or(1), origin);
    co_return;
}

namespace detail {

template <typename transaction_type>
ruvia::task<queued_dns_sync> queued_sync(transaction_type& transaction, std::string_view tenant_id,
    std::string marker_id) {
    const auto rows = co_await transaction.query(
        "SELECT COALESCE(subtask_id::text, '') FROM sys_sync_task WHERE tenant_id = $1 AND id = $2",
        tenant_id, std::string_view(marker_id));
    co_return queued_dns_sync{.marker_id_ = std::move(marker_id),
        .subtask_id_ = rows.empty() ? std::string{} : std::string(rows.front()[0].value().value_or(""))};
}

}  // namespace detail

template <typename runtime_type>
ruvia::task<queued_dns_sync> persist_dns_challenges(
    runtime_type& context, const service::sync_runtime::running_marker_lease& certificate_marker,
    const dns_challenge_config_view& config, const std::vector<dns_challenge_spec>& specs) {
    auto transaction = co_await context.db().begin_transaction();
    const auto rows = co_await transaction.query(
        "SELECT runtime::text, desired_revision FROM sys_dns_zone WHERE tenant_id = $1 AND id = "
        "$2 AND deleted_at IS NULL LIMIT 1 FOR UPDATE",
        certificate_marker.marker_.tenant_id_, config.dns_zone_id_);
    if (rows.empty()) {
        throw std::runtime_error("DNS-01 托管域名不存在");
    }
    const auto runtime = service::dns_sync::parse_stored_runtime(
        rows.front()[0].value().value_or("{}"), {.resource_ = context.pool()});
    if (!runtime) {
        throw std::runtime_error("DNS runtime 损坏");
    }

    auto next_runtime = *runtime;
    std::erase_if(next_runtime.challenge_records_, [&](const auto& challenge) {
        return challenge.certificate_id_ == config.certificate_id_;
    });
    const auto ids = co_await transaction.query(
        "SELECT gen_random_uuid()::TEXT FROM generate_series(1, $1::BIGINT)",
        static_cast<std::int64_t>(specs.size()));
    if (ids.size() != specs.size()) {
        throw std::runtime_error("DNS-01 Challenge ID 生成失败");
    }
    for (std::size_t index = 0; index < specs.size(); ++index) {
        next_runtime.challenge_records_.push_back({
            .id_ = std::string(ids[index][0].value().value_or("")),
            .name_ = specs[index].name_,
            .content_ = specs[index].content_,
            .ttl_ = config.minimum_record_ttl_,
            .certificate_id_ = std::string(config.certificate_id_),
        });
    }
    const auto runtime_output =
        service::dns_sync::to_output(next_runtime, {.resource_ = context.pool()});
    const auto runtime_json = ruvia::to_json(runtime_output, {.resource_ = context.pool()});
    const auto updated = co_await transaction.query(
        "UPDATE sys_dns_zone SET runtime = $1::jsonb, desired_revision = desired_revision + 1, "
        "sync_status = 'pending', last_error = NULL, updated_at = NOW() WHERE tenant_id = $2 "
        "AND id = $3 AND desired_revision = $4 AND deleted_at IS NULL RETURNING desired_revision",
        std::string_view(runtime_json), certificate_marker.marker_.tenant_id_, config.dns_zone_id_,
        rows.front()[1].template as<std::int64_t>().value_or(0));
    if (updated.empty()) {
        throw std::runtime_error("DNS-01 托管域名版本已变化");
    }
    auto origin =
        service::operation::work_origin::caused_by(config.owner_, service::operation::subtask_purpose::dns_challenge);
    auto queued = co_await detail::queued_sync(transaction, certificate_marker.marker_.tenant_id_,
        co_await service::dns_sync::enqueue_zone_revision(transaction,
            std::string(certificate_marker.marker_.tenant_id_), std::string(config.dns_zone_id_),
            updated.front()[0].template as<std::int64_t>().value_or(1), origin));
    if (!co_await service::sync_runtime::renew_running_lease(transaction, certificate_marker)) {
        throw std::runtime_error("证书同步标记 lease 已失效");
    }
    co_await transaction.commit();
    co_await service::sync_runtime::notify_origin(context, certificate_marker.marker_.tenant_id_, origin);
    co_return queued;
}

template <typename runtime_type>
ruvia::task<queued_dns_sync> retire_dns_challenges(runtime_type& context, const dns_challenge_config_view& config,
    const service::sync_runtime::running_marker_lease& certificate_marker) {
    const auto dns_zone_id = config.dns_zone_id_;
    const auto certificate_id = config.certificate_id_;
    auto transaction = co_await context.db().begin_transaction();
    const auto rows = co_await transaction.query(
        "SELECT runtime::text, desired_revision FROM sys_dns_zone WHERE tenant_id = $1 AND id = "
        "$2 AND deleted_at IS NULL LIMIT 1 FOR UPDATE",
        certificate_marker.marker_.tenant_id_, dns_zone_id);
    if (rows.empty()) {
        throw std::runtime_error("DNS-01 托管域名不存在");
    }
    const auto runtime = service::dns_sync::parse_stored_runtime(
        rows.front()[0].value().value_or("{}"), {.resource_ = context.pool()});
    if (!runtime) {
        throw std::runtime_error("DNS runtime 损坏");
    }
    auto next_runtime = *runtime;
    const auto old_size = next_runtime.challenge_records_.size();
    std::erase_if(next_runtime.challenge_records_,
        [&](const auto& challenge) { return challenge.certificate_id_ == certificate_id; });
    if (old_size == next_runtime.challenge_records_.size()) {
        co_await transaction.commit();
        co_return queued_dns_sync{};
    }
    const auto runtime_output =
        service::dns_sync::to_output(next_runtime, {.resource_ = context.pool()});
    const auto runtime_json = ruvia::to_json(runtime_output, {.resource_ = context.pool()});
    const auto updated = co_await transaction.query(
        "UPDATE sys_dns_zone SET runtime = $1::jsonb, desired_revision = desired_revision + 1, "
        "sync_status = 'pending', last_error = NULL, updated_at = NOW() WHERE tenant_id = $2 "
        "AND id = $3 AND desired_revision = $4 AND deleted_at IS NULL RETURNING desired_revision",
        std::string_view(runtime_json), certificate_marker.marker_.tenant_id_, dns_zone_id,
        rows.front()[1].template as<std::int64_t>().value_or(0));
    if (updated.empty()) {
        throw std::runtime_error("DNS-01 托管域名版本已变化");
    }
    auto origin = service::operation::work_origin::caused_by(config.owner_,
        service::operation::subtask_purpose::dns_challenge_cleanup);
    auto queued = co_await detail::queued_sync(transaction, certificate_marker.marker_.tenant_id_,
        co_await service::dns_sync::enqueue_zone_revision(transaction,
            std::string(certificate_marker.marker_.tenant_id_), std::string(dns_zone_id),
            updated.front()[0].template as<std::int64_t>().value_or(1), origin));
    if (!co_await service::sync_runtime::renew_running_lease(transaction, certificate_marker)) {
        throw std::runtime_error("证书同步标记 lease 已失效");
    }
    co_await transaction.commit();
    co_await service::sync_runtime::notify_origin(context, certificate_marker.marker_.tenant_id_, origin);
    co_return queued;
}

inline ruvia::task<void>
wait_for_dns_marker(service::background::worker_context& context, std::string_view marker_id,
    const service::sync_runtime::running_marker_lease& certificate_marker) {
    if (marker_id.empty()) {
        co_return;
    }
    int polls = 0;
    auto db = context.db();
    while (!context.get_stop_token().stop_requested()) {
        if (polls++ % 10 == 0 &&
            !co_await service::sync_runtime::renew_running_lease(db, certificate_marker)) {
            throw std::runtime_error("证书同步标记 lease 已失效");
        }
        const auto rows = co_await db.query(
            "SELECT is_done, is_ok, error FROM sys_sync_task WHERE tenant_id = $1 AND id = $2 "
            "LIMIT 1",
            certificate_marker.marker_.tenant_id_, marker_id);
        if (rows.empty()) {
            throw std::runtime_error("DNS 同步标记不存在");
        }
        if (rows.front()[0].as<bool>().value_or(false)) {
            if (rows.front()[1].as<bool>().value_or(false)) {
                co_return;
            }
            throw std::runtime_error("DNS 同步失败：" +
                                     std::string(rows.front()[2].value().value_or("执行失败")));
        }
        if (co_await ruvia::sleep_for(context.worker(), std::chrono::milliseconds(500),
                context.get_stop_token()) ==
            ruvia::timer_sleep_result::stop_requested) {
            break;
        }
    }
    throw std::runtime_error("DNS 同步等待已停止");
}

}  // namespace service::certificate_issuance
