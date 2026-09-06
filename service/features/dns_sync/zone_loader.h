#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ruvia/core/Task.h>

#include "service/features/background/worker_pool.h"
#include "service/features/dns/driver.h"
#include "service/features/dns/provider_config.h"
#include "service/features/dns_sync/queue.h"
#include "service/features/dns_sync/snapshot.h"
#include "service/features/sync_runtime/state.h"
#include "service/features/dns_sync/task.h"

namespace service::dns_sync::detail {

struct ZoneSyncState final {
    ZoneRuntimeData runtime;
    ZoneConfigData config;
    ZoneConfigData desired;
    service::dns::DnsProviderConfigData providerConfig;
    std::string domain;
    std::string provider;
    std::string accountId;
    std::int64_t revision;
    std::int64_t configRevision;
    bool deleted;
};

struct RemoteZoneData final {
    std::string id;
    std::vector<service::dns::ProviderRecord> records;
    std::vector<service::dns::ProviderLine> lines;
};

inline ruvia::Task<std::optional<ZoneSyncState>>
loadZoneSyncState(service::background::WorkerContext& context, const DnsTask& task,
                  const service::sync_runtime::RunningMarkerLease& lease) {
    if (!co_await service::sync_runtime::renewRunningLease(context.db(), lease)) {
        throw std::runtime_error("DNS 同步标记 lease 已失效");
    }
    const auto rows = co_await context.db().query(
        "SELECT zone.desired_revision, zone.domain, zone.runtime::text, zone.deleted_at IS NOT "
        "NULL, provider.provider, provider.account_id, provider.config::text, provider.status, "
        "zone.config::text, zone.revision FROM "
        "sys_dns_zone zone INNER "
        "JOIN sys_provider provider ON provider.tenant_id = zone.tenant_id AND provider.id "
        "= zone.provider_id AND provider.kind = 'dns' WHERE zone.id = $1 AND zone.tenant_id = "
        "$2 AND provider.deleted_at IS NULL LIMIT 1",
        task.resourceId, task.tenantId);
    if (rows.empty()) {
        throw std::runtime_error("DNS 聚合根或服务商不存在");
    }
    const auto& row = rows.front();
    const auto revision = row[0].as<std::int64_t>().value_or(task.version);
    if (revision != task.version) {
        auto transaction = co_await context.db().beginTransaction();
        (void)co_await service::sync_runtime::releaseRunning(transaction, lease);
        co_await transaction.commit();
        co_return std::nullopt;
    }

    const auto runtime =
        parseStoredRuntime(row[2].value().value_or("{}"), {.resource = context.resource()});
    if (!runtime) {
        throw std::runtime_error("DNS runtime 损坏");
    }
    const bool deleted = row[3].as<bool>().value_or(false) ||
                         task.operation == service::sync_runtime::MarkerOperation::remove;
    const std::string provider = std::string(row[4].value().value_or(""));
    const std::string accountId = std::string(row[5].value().value_or(""));
    auto providerConfig =
        service::dns::parseDnsProviderConfig(row[6].value().value_or("{}"), context.resource());
    if (row[7].value().value_or("") != "verified") {
        throw std::runtime_error("DNS 服务商账号尚未通过检测");
    }

    const std::optional<ZoneConfigData> config =
        parseStored(row[8].value().value_or("{}"), {.resource = context.resource()});
    if (!config) {
        throw std::runtime_error("DNS 聚合配置损坏");
    }

    ZoneConfigData desired = *config;
    const auto projected =
        co_await loadProjectedRecords(context.db(), task.tenantId, task.resourceId);
    for (const auto& record : projected) {
        desired.records.push_back({.id = record.id,
                                   .type = record.type,
                                   .name = record.name,
                                   .content = record.content,
                                   .ttl = record.ttl,
                                   .priority = record.priority,
                                   .proxied = record.proxied,
                                   .lineCode = record.lineCode});
    }
    for (const auto& challenge : runtime->challengeRecords) {
        desired.records.push_back({.id = challenge.id,
                                   .type = "TXT",
                                   .name = challenge.name,
                                   .content = challenge.content,
                                   .ttl = challenge.ttl,
                                   .priority = std::nullopt,
                                   .proxied = false,
                                   .lineCode = "default"});
    }

    co_return ZoneSyncState{
        .runtime = std::move(*runtime),
        .config = std::move(*config),
        .desired = std::move(desired),
        .providerConfig = std::move(providerConfig),
        .domain = std::string(row[1].value().value_or("")),
        .provider = provider,
        .accountId = accountId,
        .revision = revision,
        .configRevision = row[9].as<std::int64_t>().value_or(0),
        .deleted = deleted,
    };
}

inline ruvia::Task<RemoteZoneData>
loadRemoteZoneData(service::background::WorkerContext& context,
                   const service::sync_runtime::RunningMarkerLease& lease,
                   const service::dns::DnsProviderDriver& driver, std::string_view accountId,
                   std::string_view secret, std::string_view domain) {
    const auto remoteZone = co_await driver.findZone(context, accountId, secret, domain);
    if (!co_await service::sync_runtime::renewRunningLease(context.db(), lease)) {
        throw std::runtime_error("DNS 同步标记 lease 已失效");
    }
    auto records = co_await driver.listRecords(context, accountId, secret, remoteZone.id, domain);
    if (!co_await service::sync_runtime::renewRunningLease(context.db(), lease)) {
        throw std::runtime_error("DNS 同步标记 lease 已失效");
    }
    auto lines = co_await driver.listLines(context, accountId, secret, domain);
    co_return RemoteZoneData{
        .id = remoteZone.id,
        .records = std::move(records),
        .lines = std::move(lines),
    };
}

} // namespace service::dns_sync::detail
