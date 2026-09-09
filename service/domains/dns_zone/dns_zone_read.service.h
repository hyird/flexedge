#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <ruvia/core/Task.h>
#include <ruvia/web/Context.h>
#include <ruvia/web/db/Db.h>

#include "service/common/domain_name.h"
#include "service/common/http.h"
#include "service/domains/dns_zone/dns_zone.error.h"
#include "service/domains/dns_zone/dns_zone.types.h"
#include "service/features/dns/provider_runtime.h"
#include "service/features/dns_sync/mapper.h"

namespace service::dns_zone {

class DnsZoneReadService final {
  public:
    ruvia::Task<DnsZoneOptionListDataDto> options(auto& c, const std::string& tenantId,
                                                  const std::optional<std::string>& keyword,
                                                  const std::optional<std::string>& ownerOf,
                                                  const std::optional<bool>& available,
                                                  bool all = false) const {
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
                (ownerOf ? (all ? " ORDER BY length(zone.domain) DESC, zone.sort DESC"
                               : " ORDER BY length(zone.domain) DESC, zone.sort DESC LIMIT 20")
                         : (all ? " ORDER BY zone.sort DESC" : " ORDER BY zone.sort DESC LIMIT 20")),
            params);
        DnsZoneOptionListDataDto result(c);
        auto& items = result.template ensure<"list">();
        for (const auto& row : rows) {
            auto& item = items.emplace_back(c);
            item.template set<"id">(row[0].value().value_or(""));
            item.template set<"domain">(row[1].value().value_or(""));
            item.template set<"dnsProvider">(row[2].value().value_or(""));
            item.template set<"dnsProviderName">(row[3].value().value_or(""));
            item.template set<"syncStatus">(row[4].value().value_or("pending"));
            item.template set<"available">(row[5].template as<bool>().value_or(false));
        }
        co_return result;
    }

    ruvia::Task<AvailableDnsZoneListDataDto>
    available(auto& c, const std::string& tenantId, const std::string& providerId) const {
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
        auto& items = result.template ensure<"list">();
        for (const auto& zone : zones) {
            if (managed.contains(zone.domain)) {
                continue;
            }
            auto& item = items.emplace_back(c);
            item.template set<"domain">(zone.domain);
            item.template set<"status">(zone.status);
        }
        co_return result;
    }

    ruvia::Task<DnsZonePageDataDto> list(auto& c, const std::string& tenantId,
                                         std::int64_t page, std::int64_t pageSize,
                                         std::int64_t skip,
                                         const std::optional<std::string>& keyword,
                                         const std::optional<std::string>& providerId) const {
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
                                             : countRows.front()[0].template as<std::int64_t>().value_or(0);
        const auto rows =
            co_await c.db().query(selectColumns() + where + " ORDER BY zone.sort DESC LIMIT " +
                                      std::to_string(pageSize) + " OFFSET " + std::to_string(skip),
                                  params);
        DnsZonePageDataDto result(c);
        result.template set<"total">(total);
        result.template set<"page">(page);
        result.template set<"pageSize">(pageSize);
        result.template set<"totalPages">(pageSize > 0 ? (total + pageSize - 1) / pageSize : 0);
        auto& items = result.template ensure<"list">();
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

    ruvia::Task<DnsZoneDto> get(auto& c, const std::string& tenantId,
                                const std::string& id) const {
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

  private:
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
    static void fillDnsZone(auto& c, Dto& item, const Row& row,
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
    toConfig(auto& c, const service::dns_sync::ZoneConfigData& input) {
        return service::dns_sync::toOutput(input, {.resource = c.resource()});
    }

    static void fillRuntimeLines(auto& c, DnsZoneRuntimeDto& output,
                                 const service::dns_sync::ZoneRuntimeData& input) {
        auto& lines = output.template ensure<"lines">();
        for (const auto& line : input.lines) {
            if (!line.code || !line.name) {
                continue;
            }
            auto& item = lines.emplace_back(c);
            item.template set<"code">(*line.code);
            item.template set<"name">(*line.name);
            item.template set<"displayName">(line.displayName ? *line.displayName : *line.name);
            item.template set<"status">(line.status ? *line.status : std::string_view{"enabled"});
        }
    }

    static void
    fillProjectedRecords(auto& c, DnsZoneRuntimeDto& output,
                         const std::vector<service::dns_sync::SnapshotRecord>& projected) {
        auto& projectedRecords = output.template ensure<"projectedRecords">();
        for (const auto& record : projected) {
            auto& item = projectedRecords.emplace_back(c);
            item.template set<"id">(record.id);
            item.template set<"type">(record.type);
            item.template set<"name">(record.name);
            item.template set<"content">(record.content);
            item.template set<"ttl">(record.ttl);
            item.template set<"proxied">(record.proxied);
            item.template set<"lineCode">(record.lineCode);
            if (record.priority) {
                item.template set<"priority">(*record.priority);
            }
        }
    }

    static void fillRecordStates(auto& c, DnsZoneRuntimeDto& output,
                                 const service::dns_sync::ZoneRuntimeData& input) {
        auto& states = output.template ensure<"recordStates">();
        for (const auto& state : input.recordStates) {
            if (!state.id) {
                continue;
            }
            auto& item = states.emplace_back(c);
            item.template set<"id">(*state.id);
            item.template set<"syncStatus">(state.syncStatus ? *state.syncStatus
                                                    : std::string_view{"pending"});
            item.template set<"syncedRevision">(state.syncedRevision ? *state.syncedRevision : 0);
            if (state.lastError) {
                item.template set<"lastError">(*state.lastError);
            }
        }
    }

    static void fillConflicts(auto& c, DnsZoneRuntimeDto& output,
                              const service::dns_sync::ZoneRuntimeData& input) {
        auto& conflicts = output.template ensure<"conflicts">();
        for (const auto& conflict : input.conflicts) {
            if (!conflict.id || !conflict.type || !conflict.name || !conflict.lineCode ||
                !conflict.localContent || !conflict.remoteContent) {
                continue;
            }
            auto& item = conflicts.emplace_back(c);
            item.template set<"id">(*conflict.id);
            item.template set<"type">(*conflict.type);
            item.template set<"name">(*conflict.name);
            item.template set<"lineCode">(*conflict.lineCode);
            item.template set<"localContent">(*conflict.localContent);
            item.template set<"remoteContent">(*conflict.remoteContent);
        }
    }

    static DnsZoneRuntimeDto
    toPublicRuntime(auto& c, const service::dns_sync::ZoneRuntimeData& input,
                    const std::vector<service::dns_sync::SnapshotRecord>& projected) {
        DnsZoneRuntimeDto output(c);
        output.template set<"recordsImported">(input.recordsImported);
        if (input.linesSyncedAt) {
            output.template set<"linesSyncedAt">(*input.linesSyncedAt);
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
};

inline const DnsZoneReadService& dnsZoneReadService() {
    static const DnsZoneReadService service;
    return service;
}

} // namespace service::dns_zone
