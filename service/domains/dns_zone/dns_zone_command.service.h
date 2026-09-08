#pragma once

#include "service/features/sync_event/fanout.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>

#include <ruvia/core/Task.h>
#include <ruvia/web/Context.h>
#include <ruvia/web/ModelJson.h>
#include <ruvia/web/db/DbTransaction.h>

#include "service/common/database.h"
#include "service/common/http.h"
#include "service/domains/dns_zone/dns_zone.error.h"
#include "service/domains/dns_zone/dns_zone.types.h"
#include "service/features/dns_sync/queue.h"
#include "service/features/dns_sync/snapshot.h"

namespace service::dns_zone {

class DnsZoneCommandService final {
  public:
    ruvia::Task<void> create(ruvia::Context& c, const std::string& tenantId,
                             const CreateDnsZoneBody& body) {
        const auto& providerIdInput = body.get<"dnsProviderId">();
        const auto& domainInput = body.get<"domain">();
        if (!providerIdInput || !domainInput) {
            service::common::throwAppError(service::common::kValidationErrorCode,
                                           "DNS 服务商和域名不能为空", 400);
        }
        const auto providerId = std::string(providerIdInput->view());
        const auto domain = normalizeDomain(domainInput->view());
        const std::string configJson{R"json({"records":[]})json"};

        service::dns_sync::ZoneRuntimeDto runtime({.resource = c.resource()});
        runtime.set<"recordsImported">(false);
        (void)runtime.ensure<"lines">();
        (void)runtime.ensure<"recordStates">();
        (void)runtime.ensure<"conflicts">();
        const auto runtimeJson = ruvia::toJson(runtime, {.resource = c.resource()});

        try {
            auto transaction = co_await c.db().beginTransaction();
            const auto provider = co_await transaction.query(
                "SELECT status FROM sys_provider WHERE id = $1 AND tenant_id = $2 AND kind = "
                "'dns' AND deleted_at IS NULL LIMIT 1 FOR SHARE",
                providerId, tenantId);
            if (provider.empty()) {
                service::common::throwAppError(DnsZoneError::PROVIDER_NOT_FOUND);
            }
            if (provider.front()[0].value().value_or("") != "verified") {
                service::common::throwAppError(DnsZoneError::PROVIDER_UNAVAILABLE);
            }

            const auto inserted = co_await transaction.query(
                "INSERT INTO sys_dns_zone (tenant_id, provider_id, domain, config, runtime, "
                "sync_status, revision, desired_revision, synced_revision, created_at, updated_at) "
                "VALUES ($1, $2, $3, $4::jsonb, $5::jsonb, 'pending', 1, 1, 0, NOW(), NOW()) "
                "RETURNING id, desired_revision",
                service::common::dbParams(ruvia::DbValue{tenantId}, ruvia::DbValue{providerId},
                                          ruvia::DbValue{std::string_view(domain)},
                                          ruvia::DbValue{std::string_view(configJson)},
                                          ruvia::DbValue{std::string_view(runtimeJson)}));
            co_await service::dns_sync::enqueueZoneRevision(
                transaction, tenantId, std::string(inserted.front()[0].value().value_or("")),
                inserted.front()[1].as<std::int64_t>().value_or(1));
            co_await transaction.commit();
            service::sync_event::fanout::hub().publish(tenantId);
        } catch (const ruvia::DbError& error) {
            if (service::common::isUniqueConstraintViolation(error, "uk_dns_zone_domain")) {
                service::common::throwAppError(DnsZoneError::EXISTS);
            }
            throw;
        }
        co_return;
    }

    ruvia::Task<void>
    updateConfig(ruvia::Context& c, const std::string& tenantId, const std::string& id,
                 std::int64_t expectedRevision,
                 const ruvia::ValidatedJson<service::dns_sync::ZoneConfigInput>& config) {
        auto transaction = co_await c.db().beginTransaction();
        const auto current = co_await transaction.query(
            "SELECT zone.revision, zone.runtime::text, provider.provider, zone.domain FROM "
            "sys_dns_zone zone "
            "INNER JOIN sys_provider provider ON provider.tenant_id = zone.tenant_id AND "
            "provider.id = zone.provider_id WHERE zone.id = $1 AND zone.tenant_id = $2 AND "
            "zone.deleted_at IS NULL AND provider.deleted_at IS NULL LIMIT 1 FOR UPDATE OF zone",
            id, tenantId);
        if (current.empty()) {
            service::common::throwAppError(DnsZoneError::NOT_FOUND);
        }
        if (current.front()[0].as<std::int64_t>().value_or(0) != expectedRevision) {
            service::common::throwAppError(DnsZoneError::REVISION_CONFLICT);
        }
        const auto normalizedConfig = service::dns_sync::normalize(config.value());
        if (!normalizedConfig) {
            service::common::throwAppError(service::common::kValidationErrorCode,
                                           "records 不能为空且每条记录必须完整", 400);
        }
        validateRecordLines(c, *normalizedConfig, current.front()[1].value().value_or("{}"));
        validateRecordTtls(*normalizedConfig, current.front()[2].value().value_or(""));
        co_await validateSystemManagedRecords(
            transaction, tenantId, id, current.front()[3].value().value_or(""),
            current.front()[1].value().value_or("{}"), *normalizedConfig);
        const auto configJson = serializeConfig(c, *normalizedConfig);
        const auto rows = co_await transaction.query(
            "UPDATE sys_dns_zone SET config = $1::jsonb, revision = revision + 1, "
            "desired_revision = desired_revision + 1, sync_status = 'pending', last_error = NULL, "
            "updated_at = NOW() WHERE id = $2 AND tenant_id = $3 AND revision = $4 AND "
            "deleted_at IS NULL RETURNING revision, desired_revision",
            service::common::dbParams(ruvia::DbValue{std::string_view(configJson)},
                                      ruvia::DbValue{id}, ruvia::DbValue{tenantId},
                                      ruvia::DbValue{expectedRevision}));
        if (rows.empty()) {
            service::common::throwAppError(DnsZoneError::REVISION_CONFLICT);
        }
        const auto desiredRevision = rows.front()[1].as<std::int64_t>().value_or(1);
        co_await service::dns_sync::enqueueZoneRevision(transaction, tenantId, id, desiredRevision);
        co_await transaction.commit();
        service::sync_event::fanout::hub().publish(tenantId);
        co_return;
    }

    ruvia::Task<void> requestSync(ruvia::Context& c, const std::string& tenantId,
                                  const std::string& id, std::string_view conflictPolicy) {
        auto transaction = co_await c.db().beginTransaction();
        const auto rows = co_await transaction.query(
            "UPDATE sys_dns_zone SET desired_revision = desired_revision + 1, sync_status = "
            "'pending', last_error = NULL, updated_at = NOW() WHERE id = $1 AND tenant_id = $2 "
            "AND deleted_at IS NULL RETURNING desired_revision",
            id, tenantId);
        if (rows.empty()) {
            service::common::throwAppError(DnsZoneError::NOT_FOUND);
        }
        co_await service::dns_sync::enqueueZoneRevision(
            transaction, tenantId, id, rows.front()[0].as<std::int64_t>().value_or(1),
            conflictPolicy == "remote"  ? service::sync_runtime::MarkerOperation::syncRemote
            : conflictPolicy == "local" ? service::sync_runtime::MarkerOperation::syncLocal
                                        : service::sync_runtime::MarkerOperation::sync);
        co_await transaction.commit();
        service::sync_event::fanout::hub().publish(tenantId);
        co_return;
    }

    ruvia::Task<void> remove(ruvia::Context& c, const std::string& tenantId, const std::string& id,
                             std::int64_t expectedRevision) {
        auto transaction = co_await c.db().beginTransaction();
        const auto zone = co_await transaction.query(
            "SELECT revision, desired_revision FROM sys_dns_zone WHERE id = $1 AND tenant_id = "
            "$2 AND deleted_at IS NULL LIMIT 1 FOR UPDATE",
            id, tenantId);
        if (zone.empty()) {
            service::common::throwAppError(DnsZoneError::NOT_FOUND);
        }
        if (zone.front()[0].as<std::int64_t>().value_or(0) != expectedRevision) {
            service::common::throwAppError(DnsZoneError::REVISION_CONFLICT);
        }
        const auto references = co_await transaction.query(
            "SELECT id FROM sys_certificate WHERE tenant_id = $1 AND dns_zone_id = $2 AND "
            "deleted_at IS NULL UNION ALL SELECT id FROM sys_cluster WHERE tenant_id = $1 AND "
            "dns_zone_id = $2 AND deleted_at IS NULL UNION ALL SELECT website_id FROM "
            "sys_website_domain_claim WHERE tenant_id = $1 AND dns_zone_id = $2 LIMIT 1",
            tenantId, id);
        if (!references.empty()) {
            service::common::throwAppError(DnsZoneError::IN_USE);
        }

        const auto revision = expectedRevision + 1;
        const auto desiredRevision = zone.front()[1].as<std::int64_t>().value_or(1) + 1;
        (void)co_await transaction.execute(
            "UPDATE sys_dns_zone SET revision = $1, desired_revision = $2, sync_status = "
            "'pending', "
            "deleted_at = NOW(), updated_at = NOW() WHERE id = $3 AND tenant_id = $4 AND "
            "revision = $5",
            revision, desiredRevision, id, tenantId, expectedRevision);
        (void)co_await service::dns_sync::enqueueZoneDeletion(transaction, tenantId, id,
                                                              desiredRevision);
        co_await transaction.commit();
        service::sync_event::fanout::hub().publish(tenantId);
        co_return;
    }

  private:
    static void validateRecordLines(ruvia::Context& c,
                                    const service::dns_sync::ZoneConfigData& config,
                                    std::string_view runtimeJson) {
        const auto runtime =
            service::dns_sync::parseStoredRuntime(runtimeJson, {.resource = c.resource()});
        if (!runtime) {
            throwCorruptConfig();
        }
        std::unordered_set<std::string> enabledLines;
        for (const auto& line : runtime->lines) {
            if (!line.code || !line.status) {
                throwCorruptConfig();
            }
            if (*line.status == "enabled") {
                enabledLines.emplace(*line.code);
            }
        }
        for (const auto& record : config.records) {
            if (!enabledLines.contains(record.lineCode)) {
                service::common::throwAppError(DnsZoneError::DNS_LINE_INVALID);
            }
        }
    }

    static void validateRecordTtls(const service::dns_sync::ZoneConfigData& config,
                                   std::string_view provider) {
        if (provider != "aliyun") {
            return;
        }
        for (const auto& record : config.records) {
            if (record.ttl < 600) {
                service::common::throwAppError(service::common::kValidationErrorCode,
                                               "阿里云 DNS 的 TTL 必须在 600 到 86400 秒之间", 422);
            }
        }
    }

    static ruvia::Task<void>
    validateSystemManagedRecords(ruvia::DbTransaction& transaction, const std::string& tenantId,
                                 const std::string& zoneId, std::string_view zoneDomain,
                                 std::string_view runtimeJson,
                                 const service::dns_sync::ZoneConfigData& config) {
        const auto projected =
            co_await service::dns_sync::loadProjectedRecords(transaction, tenantId, zoneId);
        std::unordered_set<std::string> systemIds;
        std::unordered_set<std::string> trafficHostnames;
        systemIds.reserve(projected.size());
        trafficHostnames.reserve(projected.size());
        for (const auto& record : projected) {
            systemIds.emplace(record.id);
            if (service::dns_sync::isTrafficRecord(record.type)) {
                trafficHostnames.emplace(
                    service::dns_sync::recordHostname(record.name, zoneDomain));
            }
        }

        const auto runtime = service::dns_sync::parseStoredRuntime(runtimeJson);
        if (!runtime) {
            throwCorruptConfig();
        }
        std::unordered_set<std::string> challengeHostnames;
        challengeHostnames.reserve(runtime->challengeRecords.size());
        for (const auto& record : runtime->challengeRecords) {
            systemIds.emplace(record.id);
            challengeHostnames.emplace(service::dns_sync::recordHostname(record.name, zoneDomain));
        }
        for (const auto& record : config.records) {
            const auto hostname = service::dns_sync::recordHostname(record.name, zoneDomain);
            if (systemIds.contains(record.id) ||
                (service::dns_sync::isTrafficRecord(record.type) &&
                 trafficHostnames.contains(hostname)) ||
                (record.type == "TXT" && challengeHostnames.contains(hostname))) {
                service::common::throwAppError(DnsZoneError::SYSTEM_MANAGED_RECORD);
            }
        }
        co_return;
    }

    static std::string serializeConfig(ruvia::Context& c,
                                       const service::dns_sync::ZoneConfigData& input) {
        return std::string{
            ruvia::toJson(service::dns_sync::toOutput(input, {.resource = c.resource()}),
                          {.resource = c.resource()})};
    }

    [[noreturn]] static void throwCorruptConfig() {
        service::common::throwAppError(service::common::kServerErrorCode, "聚合配置损坏", 500);
    }

    static std::string normalizeDomain(std::string_view input) {
        const auto begin = std::find_if_not(input.begin(), input.end(),
                                            [](unsigned char ch) { return std::isspace(ch) != 0; });
        const auto end = std::find_if_not(input.rbegin(), input.rend(), [](unsigned char ch) {
                             return std::isspace(ch) != 0;
                         }).base();
        std::string result = begin < end ? std::string(begin, end) : std::string{};
        std::transform(result.begin(), result.end(), result.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return result;
    }
};

inline DnsZoneCommandService& dnsZoneCommandService() {
    static DnsZoneCommandService service;
    return service;
}

} // namespace service::dns_zone
