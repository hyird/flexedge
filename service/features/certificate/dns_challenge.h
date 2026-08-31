#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <ruvia/core/Task.h>
#include <ruvia/core/Timer.h>

#include "service/features/background/worker_pool.h"
#include "service/features/dns_sync/queue.h"
#include "service/features/dns_sync/snapshot.h"
#include "service/features/sync_runtime/state.h"

namespace service::certificate_issuance {

struct DnsChallengeConfigView final {
    std::string_view dnsZoneId;
    std::string_view zoneDomain;
    std::string_view certificateId;
    std::int64_t minimumRecordTtl;
};

struct DnsChallengeSpec final {
    std::string name;
    std::string content;
};

inline ruvia::Task<void> retireDnsChallengesForDeletedCertificate(ruvia::DbTransaction& transaction,
                                                                  std::string_view tenantId,
                                                                  std::string_view dnsZoneId,
                                                                  std::string_view certificateId) {
    const auto rows = co_await transaction.query(
        "SELECT runtime::text, desired_revision FROM sys_dns_zone WHERE tenant_id = $1 AND id = "
        "$2 AND deleted_at IS NULL LIMIT 1 FOR UPDATE",
        tenantId, dnsZoneId);
    if (rows.empty()) {
        co_return;
    }
    const auto runtime =
        service::dns_sync::parseStoredRuntime(rows.front()[0].value().value_or("{}"));
    if (!runtime) {
        throw std::runtime_error("DNS runtime 损坏");
    }
    auto nextRuntime = *runtime;
    const auto oldSize = nextRuntime.challengeRecords.size();
    std::erase_if(nextRuntime.challengeRecords,
                  [&](const auto& challenge) { return challenge.certificateId == certificateId; });
    if (oldSize == nextRuntime.challengeRecords.size()) {
        co_return;
    }
    const auto runtimeJson = ruvia::toJson(service::dns_sync::toOutput(nextRuntime));
    const auto updated = co_await transaction.query(
        "UPDATE sys_dns_zone SET runtime = $1::jsonb, desired_revision = desired_revision + 1, "
        "sync_status = 'pending', last_error = NULL, updated_at = NOW() WHERE tenant_id = $2 "
        "AND id = $3 AND desired_revision = $4 AND deleted_at IS NULL RETURNING desired_revision",
        std::string_view(runtimeJson), tenantId, dnsZoneId,
        rows.front()[1].as<std::int64_t>().value_or(0));
    if (updated.empty()) {
        throw std::runtime_error("DNS-01 托管域名版本已变化");
    }
    (void)co_await service::dns_sync::enqueueZoneRevision(
        transaction, std::string(tenantId), std::string(dnsZoneId),
        updated.front()[0].as<std::int64_t>().value_or(1));
    co_return;
}

template <typename Runtime>
ruvia::Task<std::string> persistDnsChallenges(
    Runtime& context, const service::sync_runtime::RunningMarkerLease& certificateMarker,
    const DnsChallengeConfigView& config, const std::vector<DnsChallengeSpec>& specs) {
    auto transaction = co_await context.db().beginTransaction();
    const auto rows = co_await transaction.query(
        "SELECT runtime::text, desired_revision FROM sys_dns_zone WHERE tenant_id = $1 AND id = "
        "$2 AND deleted_at IS NULL LIMIT 1 FOR UPDATE",
        certificateMarker.marker.tenantId, config.dnsZoneId);
    if (rows.empty()) {
        throw std::runtime_error("DNS-01 托管域名不存在");
    }
    const auto runtime = service::dns_sync::parseStoredRuntime(
        rows.front()[0].value().value_or("{}"), {.resource = context.resource()});
    if (!runtime) {
        throw std::runtime_error("DNS runtime 损坏");
    }

    auto nextRuntime = *runtime;
    std::erase_if(nextRuntime.challengeRecords, [&](const auto& challenge) {
        return challenge.certificateId == config.certificateId;
    });
    const auto ids = co_await transaction.query(
        "SELECT gen_random_uuid()::TEXT FROM generate_series(1, $1::BIGINT)",
        static_cast<std::int64_t>(specs.size()));
    if (ids.size() != specs.size()) {
        throw std::runtime_error("DNS-01 Challenge ID 生成失败");
    }
    for (std::size_t index = 0; index < specs.size(); ++index) {
        nextRuntime.challengeRecords.push_back({
            .id = std::string(ids[index][0].value().value_or("")),
            .name = specs[index].name,
            .content = specs[index].content,
            .ttl = config.minimumRecordTtl,
            .certificateId = std::string(config.certificateId),
        });
    }
    const auto runtimeOutput =
        service::dns_sync::toOutput(nextRuntime, {.resource = context.resource()});
    const auto runtimeJson = ruvia::toJson(runtimeOutput, {.resource = context.resource()});
    const auto updated = co_await transaction.query(
        "UPDATE sys_dns_zone SET runtime = $1::jsonb, desired_revision = desired_revision + 1, "
        "sync_status = 'pending', last_error = NULL, updated_at = NOW() WHERE tenant_id = $2 "
        "AND id = $3 AND desired_revision = $4 AND deleted_at IS NULL RETURNING desired_revision",
        std::string_view(runtimeJson), certificateMarker.marker.tenantId, config.dnsZoneId,
        rows.front()[1].template as<std::int64_t>().value_or(0));
    if (updated.empty()) {
        throw std::runtime_error("DNS-01 托管域名版本已变化");
    }
    const auto markerId = co_await service::dns_sync::enqueueZoneRevision(
        transaction, std::string(certificateMarker.marker.tenantId), std::string(config.dnsZoneId),
        updated.front()[0].template as<std::int64_t>().value_or(1));
    if (!co_await service::sync_runtime::renewRunningLease(transaction, certificateMarker)) {
        throw std::runtime_error("证书同步标记 lease 已失效");
    }
    co_await transaction.commit();
    co_return markerId;
}

template <typename Runtime>
ruvia::Task<std::string>
retireDnsChallenges(Runtime& context, std::string_view dnsZoneId, std::string_view certificateId,
                    const service::sync_runtime::RunningMarkerLease& certificateMarker) {
    auto transaction = co_await context.db().beginTransaction();
    const auto rows = co_await transaction.query(
        "SELECT runtime::text, desired_revision FROM sys_dns_zone WHERE tenant_id = $1 AND id = "
        "$2 AND deleted_at IS NULL LIMIT 1 FOR UPDATE",
        certificateMarker.marker.tenantId, dnsZoneId);
    if (rows.empty()) {
        throw std::runtime_error("DNS-01 托管域名不存在");
    }
    const auto runtime = service::dns_sync::parseStoredRuntime(
        rows.front()[0].value().value_or("{}"), {.resource = context.resource()});
    if (!runtime) {
        throw std::runtime_error("DNS runtime 损坏");
    }
    auto nextRuntime = *runtime;
    const auto oldSize = nextRuntime.challengeRecords.size();
    std::erase_if(nextRuntime.challengeRecords,
                  [&](const auto& challenge) { return challenge.certificateId == certificateId; });
    if (oldSize == nextRuntime.challengeRecords.size()) {
        co_await transaction.commit();
        co_return std::string{};
    }
    const auto runtimeOutput =
        service::dns_sync::toOutput(nextRuntime, {.resource = context.resource()});
    const auto runtimeJson = ruvia::toJson(runtimeOutput, {.resource = context.resource()});
    const auto updated = co_await transaction.query(
        "UPDATE sys_dns_zone SET runtime = $1::jsonb, desired_revision = desired_revision + 1, "
        "sync_status = 'pending', last_error = NULL, updated_at = NOW() WHERE tenant_id = $2 "
        "AND id = $3 AND desired_revision = $4 AND deleted_at IS NULL RETURNING desired_revision",
        std::string_view(runtimeJson), certificateMarker.marker.tenantId, dnsZoneId,
        rows.front()[1].template as<std::int64_t>().value_or(0));
    if (updated.empty()) {
        throw std::runtime_error("DNS-01 托管域名版本已变化");
    }
    const auto markerId = co_await service::dns_sync::enqueueZoneRevision(
        transaction, std::string(certificateMarker.marker.tenantId), std::string(dnsZoneId),
        updated.front()[0].template as<std::int64_t>().value_or(1));
    if (!co_await service::sync_runtime::renewRunningLease(transaction, certificateMarker)) {
        throw std::runtime_error("证书同步标记 lease 已失效");
    }
    co_await transaction.commit();
    co_return markerId;
}

inline ruvia::Task<void>
waitForDnsMarker(service::background::WorkerContext& context, std::string_view markerId,
                 const service::sync_runtime::RunningMarkerLease& certificateMarker) {
    if (markerId.empty()) {
        co_return;
    }
    int polls = 0;
    while (!context.stopToken().stopRequested()) {
        if (polls++ % 10 == 0 &&
            !co_await service::sync_runtime::renewRunningLease(context.db(), certificateMarker)) {
            throw std::runtime_error("证书同步标记 lease 已失效");
        }
        const auto rows = co_await context.db().query(
            "SELECT is_done, is_ok, error FROM sys_sync_task WHERE tenant_id = $1 AND id = $2 "
            "LIMIT 1",
            certificateMarker.marker.tenantId, markerId);
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
        if (co_await ruvia::sleepFor(context.worker(), std::chrono::milliseconds(500),
                                     context.stopToken()) ==
            ruvia::TimerSleepResult::kStopRequested) {
            break;
        }
    }
    throw std::runtime_error("DNS 同步等待已停止");
}

} // namespace service::certificate_issuance
