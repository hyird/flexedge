#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <ruvia/core/Task.h>
#include <ruvia/web/Context.h>
#include <ruvia/web/ModelJson.h>

#include "service/common/database.h"
#include "service/common/http.h"
#include "service/common/domain_name.h"
#include "service/domains/dns_zone/dns_zone.error.h"
#include "service/domains/dns_zone/dns_zone.types.h"
#include "service/features/dns/provider_runtime.h"
#include "service/features/dns_sync/queue.h"
#include "service/features/dns_sync/snapshot.h"

namespace service::dns_zone {

class DnsZoneService {
  public:
    ruvia::Task<DnsZoneOptionListDataDto> options(ruvia::Context& c, const std::string& tenantId,
                                                  const std::optional<std::string>& keyword,
                                                  const std::optional<std::string>& ownerOf,
                                                  const std::optional<bool>& available) {
        std::string where =
            " FROM sys_dns_zone zone INNER JOIN sys_provider provider ON provider.tenant_id = "
            "zone.tenant_id AND provider.id = zone.provider_id AND provider.kind = 'dns' "
            "WHERE zone.tenant_id = $1 AND zone.deleted_at IS NULL AND provider.deleted_at IS "
            "NULL";
        std::vector<ruvia::DbValue> params{ruvia::DbValue{tenantId}};
        std::optional<std::string> pattern;
        if (keyword) {
            pattern = "%" + service::common::escapeLikePattern(*keyword) + "%";
            where += " AND (zone.domain ILIKE $" + std::to_string(params.size() + 1) +
                     " OR provider.name ILIKE $" + std::to_string(params.size() + 1) + ")";
            params.emplace_back(std::string_view(*pattern));
        }
        std::optional<std::string> owner;
        if (ownerOf) {
            owner = service::common::normalizeDomainName(*ownerOf);
            if (owner->starts_with("*.")) {
                owner->erase(0, 2);
            }
            where += " AND ($" + std::to_string(params.size() + 1) + " = zone.domain OR $" +
                     std::to_string(params.size() + 1) + " LIKE '%.' || zone.domain)";
            params.emplace_back(std::string_view(*owner));
        }
        if (available) {
            where += " AND (zone.sync_status = 'synced' AND provider.status = 'verified') = $" +
                     std::to_string(params.size() + 1);
            params.emplace_back(*available);
        }
        const auto rows = co_await c.db().query(
            "SELECT zone.id, zone.domain, provider.provider, provider.name, zone.sync_status, "
            "zone.sync_status = 'synced' AND provider.status = 'verified'" +
                where +
                (ownerOf ? " ORDER BY length(zone.domain) DESC, zone.sort DESC LIMIT 20"
                         : " ORDER BY zone.sort DESC LIMIT 20"),
            params);
        DnsZoneOptionListDataDto result(c);
        auto& items = result.ensure<"list">();
        for (const auto& row : rows) {
            auto& item = items.emplace_back(c);
            item.set<"id">(row[0].value().value_or(""));
            item.set<"domain">(row[1].value().value_or(""));
            item.set<"dnsProvider">(row[2].value().value_or(""));
            item.set<"dnsProviderName">(row[3].value().value_or(""));
            item.set<"syncStatus">(row[4].value().value_or("pending"));
            item.set<"available">(row[5].as<bool>().value_or(false));
        }
        co_return result;
    }

    ruvia::Task<AvailableDnsZoneListDataDto>
    available(ruvia::Context& c, const std::string& tenantId, const std::string& providerId) {
        const auto providerRows = co_await c.db().query(
            "SELECT runtime::text FROM sys_provider WHERE id = $1 AND tenant_id = $2 AND kind "
            "= 'dns' AND status = 'verified' AND deleted_at IS NULL LIMIT 1",
            providerId, tenantId);
        if (providerRows.empty()) {
            service::common::throwAppError(DnsZoneError::PROVIDER_UNAVAILABLE);
        }

        const auto zones = service::dns::parseDnsProviderRuntime(
            providerRows.front()[0].value().value_or("{}"), c.resource());
        const auto localRows = co_await c.db().query(
            "SELECT domain FROM sys_dns_zone WHERE tenant_id = $1 AND deleted_at IS NULL",
            tenantId);
        std::unordered_set<std::string> managed;
        managed.reserve(localRows.size());
        for (const auto& row : localRows) {
            managed.emplace(row[0].value().value_or(""));
        }

        AvailableDnsZoneListDataDto result(c);
        auto& items = result.ensure<"list">();
        for (const auto& zone : zones) {
            if (managed.contains(zone.domain)) {
                continue;
            }
            auto& item = items.emplace_back(c);
            item.set<"domain">(zone.domain);
            item.set<"status">(zone.status);
        }
        co_return result;
    }

    ruvia::Task<DnsZonePageDataDto> list(ruvia::Context& c, const std::string& tenantId,
                                         std::int64_t page, std::int64_t pageSize,
                                         std::int64_t skip,
                                         const std::optional<std::string>& keyword,
                                         const std::optional<std::string>& providerId) {
        std::string where =
            " FROM sys_dns_zone zone INNER JOIN sys_provider provider ON provider.tenant_id = "
            "zone.tenant_id AND provider.id = zone.provider_id WHERE zone.tenant_id = $1 "
            "AND provider.kind = 'dns' AND "
            "zone.deleted_at IS NULL AND provider.deleted_at IS NULL";
        std::vector<ruvia::DbValue> params{ruvia::DbValue{tenantId}};
        std::optional<std::string> pattern;
        if (providerId) {
            where += " AND provider.id = $" + std::to_string(params.size() + 1);
            params.emplace_back(*providerId);
        }
        if (keyword) {
            where += " AND zone.domain ILIKE $" + std::to_string(params.size() + 1);
            pattern = "%" + service::common::escapeLikePattern(*keyword) + "%";
            params.emplace_back(std::string_view(*pattern));
        }

        const auto countRows = co_await c.db().query("SELECT COUNT(*)" + where, params);
        const auto total = countRows.empty() ? std::int64_t{0}
                                             : countRows.front()[0].as<std::int64_t>().value_or(0);
        const auto rows =
            co_await c.db().query(selectColumns() + where + " ORDER BY zone.sort DESC LIMIT " +
                                      std::to_string(pageSize) + " OFFSET " + std::to_string(skip),
                                  params);
        DnsZonePageDataDto result(c);
        result.set<"total">(total);
        result.set<"page">(page);
        result.set<"pageSize">(pageSize);
        result.set<"totalPages">(pageSize > 0 ? (total + pageSize - 1) / pageSize : 0);
        auto& items = result.ensure<"list">();
        auto db = c.db();
        std::vector<std::string> zoneIds;
        zoneIds.reserve(rows.size());
        for (const auto& row : rows) {
            zoneIds.emplace_back(row[0].value().value_or(""));
        }
        const auto projectedByZone =
            co_await service::dns_sync::loadProjectedRecordsByZone(db, tenantId, zoneIds);
        for (const auto& row : rows) {
            const auto& projected = projectedByZone.at(std::string(row[0].value().value_or("")));
            fillDnsZone(c, items.emplace_back(c), row, projected);
        }
        co_return result;
    }

    ruvia::Task<DnsZoneDto> get(ruvia::Context& c, const std::string& tenantId,
                                const std::string& id) {
        const auto rows = co_await c.db().query(
            selectColumns() +
                " FROM sys_dns_zone zone INNER JOIN sys_provider provider ON "
                "provider.tenant_id = zone.tenant_id AND provider.id = zone.provider_id "
                "WHERE zone.id = $1 AND zone.tenant_id = $2 AND "
                "provider.kind = 'dns' AND zone.deleted_at IS NULL AND provider.deleted_at IS "
                "NULL LIMIT 1",
            id, tenantId);
        if (rows.empty()) {
            service::common::throwAppError(DnsZoneError::NOT_FOUND);
        }
        auto db = c.db();
        const auto projected = co_await service::dns_sync::loadProjectedRecords(db, tenantId, id);
        DnsZoneDto result(c);
        fillDnsZone(c, result, rows.front(), projected);
        co_return result;
    }

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
            "SELECT zone.revision, zone.runtime::text, provider.provider FROM sys_dns_zone zone "
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
        co_await validateClusterManagedRecords(transaction, tenantId, id, *normalizedConfig);
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
            conflictPolicy == "remote"  ? "sync_remote"
            : conflictPolicy == "local" ? "sync_local"
                                        : "sync");
        co_await transaction.commit();
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
    validateClusterManagedRecords(ruvia::DbTransaction& transaction, const std::string& tenantId,
                                  const std::string& zoneId,
                                  const service::dns_sync::ZoneConfigData& config) {
        const auto rows = co_await transaction.query(
            "SELECT hostname_prefix FROM sys_cluster WHERE tenant_id = $1 AND dns_zone_id = $2 "
            "AND status = 'enabled' AND deleted_at IS NULL",
            tenantId, zoneId);
        std::unordered_set<std::string> hostnames;
        hostnames.reserve(rows.size());
        for (const auto& row : rows) {
            hostnames.emplace(row[0].value().value_or(""));
        }
        for (const auto& record : config.records) {
            if (service::dns_sync::isClusterManagedRecord(record, hostnames)) {
                service::common::throwAppError(DnsZoneError::CLUSTER_MANAGED_RECORD);
            }
        }
        co_return;
    }

    static std::string selectColumns() {
        return "SELECT zone.id, provider.id, provider.provider, provider.name, zone.domain, "
               "zone.sync_status, zone.revision, zone.desired_revision, zone.synced_revision, "
               "zone.config::text, zone.runtime::text, "
               "TO_CHAR(zone.last_synced_at, 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF'), "
               "zone.last_error, TO_CHAR(zone.created_at, "
               "'YYYY-MM-DD\"T\"HH24:MI:SS.USOF'), TO_CHAR(zone.updated_at, "
               "'YYYY-MM-DD\"T\"HH24:MI:SS.USOF'), "
               "(SELECT COUNT(DISTINCT claim.website_id) FROM sys_website_domain_claim claim "
               "INNER JOIN sys_website website ON website.tenant_id = claim.tenant_id AND "
               "website.id = claim.website_id WHERE claim.tenant_id = zone.tenant_id AND "
               "claim.dns_zone_id = zone.id AND website.status = 'enabled' AND "
               "website.deleted_at IS NULL)";
    }

    template <typename Dto, typename Row>
    static void fillDnsZone(ruvia::Context& c, Dto& item, const Row& row,
                            const std::vector<service::dns_sync::SnapshotRecord>& projected) {
        const auto config = service::dns_sync::parseStored(row[9].value().value_or("{}"),
                                                           {.resource = c.resource()});
        const auto runtime = service::dns_sync::parseStoredRuntime(row[10].value().value_or("{}"),
                                                                   {.resource = c.resource()});
        if (!config || !runtime) {
            throwCorruptConfig();
        }

        item.template set<"id">(row[0].value().value_or(""));
        item.template set<"dnsProviderId">(row[1].value().value_or(""));
        item.template set<"dnsProvider">(row[2].value().value_or(""));
        item.template set<"dnsProviderName">(row[3].value().value_or(""));
        item.template set<"domain">(row[4].value().value_or(""));
        item.template set<"syncStatus">(row[5].value().value_or("pending"));
        item.template set<"revision">(row[6].template as<std::int64_t>().value_or(1));
        item.template set<"desiredRevision">(row[7].template as<std::int64_t>().value_or(1));
        item.template set<"syncedRevision">(row[8].template as<std::int64_t>().value_or(0));
        item.template set<"websiteCount">(row[15].template as<std::int64_t>().value_or(0));
        item.template set<"config">(toConfig(c, *config));
        item.template set<"runtime">(toPublicRuntime(c, *runtime, projected));
        item.template set<"createdAt">(row[13].value().value_or(""));
        item.template set<"updatedAt">(row[14].value().value_or(""));
        if (const auto& lastSyncedAt = row[11].value()) {
            item.template set<"lastSyncedAt">(*lastSyncedAt);
        }
        if (const auto& lastError = row[12].value()) {
            item.template set<"lastError">(*lastError);
        }
    }

    static service::dns_sync::ZoneConfigOutput
    toConfig(ruvia::Context& c, const service::dns_sync::ZoneConfigData& input) {
        return service::dns_sync::toOutput(input, {.resource = c.resource()});
    }

    static std::string serializeConfig(ruvia::Context& c,
                                       const service::dns_sync::ZoneConfigData& input) {
        return std::string{ruvia::toJson(toConfig(c, input), {.resource = c.resource()})};
    }

    static void fillRuntimeLines(ruvia::Context& c, DnsZoneRuntimeDto& output,
                                 const service::dns_sync::ZoneRuntimeData& input) {
        auto& lines = output.ensure<"lines">();
        for (const auto& line : input.lines) {
            if (!line.code || !line.name) {
                continue;
            }
            auto& item = lines.emplace_back(c);
            item.set<"code">(*line.code);
            item.set<"name">(*line.name);
            item.set<"displayName">(line.displayName ? *line.displayName : *line.name);
            item.set<"status">(line.status ? *line.status : std::string_view{"enabled"});
        }
    }

    static void
    fillProjectedRecords(ruvia::Context& c, DnsZoneRuntimeDto& output,
                         const std::vector<service::dns_sync::SnapshotRecord>& projected) {
        auto& projectedRecords = output.ensure<"projectedRecords">();
        for (const auto& record : projected) {
            auto& item = projectedRecords.emplace_back(c);
            item.set<"id">(record.id);
            item.set<"type">(record.type);
            item.set<"name">(record.name);
            item.set<"content">(record.content);
            item.set<"ttl">(record.ttl);
            item.set<"proxied">(record.proxied);
            item.set<"lineCode">(record.lineCode);
            if (record.priority) {
                item.set<"priority">(*record.priority);
            }
        }
    }

    static void fillRecordStates(ruvia::Context& c, DnsZoneRuntimeDto& output,
                                 const service::dns_sync::ZoneRuntimeData& input) {
        auto& states = output.ensure<"recordStates">();
        for (const auto& state : input.recordStates) {
            if (!state.id) {
                continue;
            }
            auto& item = states.emplace_back(c);
            item.set<"id">(*state.id);
            item.set<"syncStatus">(state.syncStatus ? *state.syncStatus
                                                    : std::string_view{"pending"});
            item.set<"syncedRevision">(state.syncedRevision ? *state.syncedRevision : 0);
            if (state.lastError) {
                item.set<"lastError">(*state.lastError);
            }
        }
    }

    static void fillConflicts(ruvia::Context& c, DnsZoneRuntimeDto& output,
                              const service::dns_sync::ZoneRuntimeData& input) {
        auto& conflicts = output.ensure<"conflicts">();
        for (const auto& conflict : input.conflicts) {
            if (!conflict.id || !conflict.type || !conflict.name || !conflict.lineCode ||
                !conflict.localContent || !conflict.remoteContent) {
                continue;
            }
            auto& item = conflicts.emplace_back(c);
            item.set<"id">(*conflict.id);
            item.set<"type">(*conflict.type);
            item.set<"name">(*conflict.name);
            item.set<"lineCode">(*conflict.lineCode);
            item.set<"localContent">(*conflict.localContent);
            item.set<"remoteContent">(*conflict.remoteContent);
        }
    }

    static DnsZoneRuntimeDto
    toPublicRuntime(ruvia::Context& c, const service::dns_sync::ZoneRuntimeData& input,
                    const std::vector<service::dns_sync::SnapshotRecord>& projected) {
        DnsZoneRuntimeDto output(c);
        output.set<"recordsImported">(input.recordsImported);
        if (input.linesSyncedAt) {
            output.set<"linesSyncedAt">(*input.linesSyncedAt);
        }
        fillRuntimeLines(c, output, input);
        fillProjectedRecords(c, output, projected);
        fillRecordStates(c, output, input);
        fillConflicts(c, output, input);
        return output;
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

inline DnsZoneService& dnsZoneService() {
    static DnsZoneService service;
    return service;
}

} // namespace service::dns_zone
