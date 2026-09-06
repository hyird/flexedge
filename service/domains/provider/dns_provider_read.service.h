#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <ruvia/core/Task.h>
#include <ruvia/web/Context.h>
#include <ruvia/web/db/Db.h>

#include "service/common/http.h"
#include "service/domains/provider/dns_provider.error.h"
#include "service/domains/provider/dns_provider.types.h"
#include "service/features/dns/provider_config.h"

namespace service::provider {

class DnsProviderReadService final {
  public:
    ruvia::Task<DnsProviderPageDataDto> list(ruvia::Context& c, const std::string& tenantId,
                                             std::int64_t page, std::int64_t pageSize,
                                             std::int64_t skip,
                                             const std::optional<std::string>& keyword,
                                             const std::optional<std::string>& status) const {
        std::string where =
            " FROM sys_provider provider WHERE provider.tenant_id = $1 AND provider.kind = "
            "'dns' AND provider.deleted_at IS NULL";
        std::vector<ruvia::DbValue> params{ruvia::DbValue{tenantId}};
        std::optional<std::string> pattern;
        if (status) {
            where += " AND provider.status = $" + std::to_string(params.size() + 1);
            params.emplace_back(std::string_view(*status));
        }
        if (keyword) {
            pattern = "%" + service::common::escapeLikePattern(*keyword) + "%";
            const auto placeholder = "$" + std::to_string(params.size() + 1);
            where += " AND (provider.name ILIKE " + placeholder + " OR provider.account_id ILIKE " +
                     placeholder + ")";
            params.emplace_back(std::string_view(*pattern));
        }
        const auto countRows = co_await c.db().query("SELECT COUNT(*)" + where, params);
        const auto total = countRows.empty() ? std::int64_t{0}
                                             : countRows.front()[0].as<std::int64_t>().value_or(0);
        const auto rows =
            co_await c.db().query(selectColumns() + where + " ORDER BY provider.sort DESC LIMIT " +
                                      std::to_string(pageSize) + " OFFSET " + std::to_string(skip),
                                  params);

        DnsProviderPageDataDto result(c);
        result.set<"total">(total);
        result.set<"page">(page);
        result.set<"pageSize">(pageSize);
        result.set<"totalPages">(pageSize > 0 ? (total + pageSize - 1) / pageSize : 0);
        auto& items = result.ensure<"list">();
        for (const auto& row : rows) {
            fill(items.emplace_back(c), parseRow(c, row));
        }
        co_return result;
    }

    ruvia::Task<DnsProviderDto> get(ruvia::Context& c, const std::string& tenantId,
                                    const std::string& id) const {
        const auto rows = co_await c.db().query(
            selectColumns() + " FROM sys_provider provider WHERE provider.id = $1 AND "
                              "provider.tenant_id = $2 AND provider.kind = 'dns' AND "
                              "provider.deleted_at IS NULL LIMIT 1",
            id, tenantId);
        if (rows.empty()) {
            service::common::throwAppError(DnsProviderError::NOT_FOUND);
        }
        DnsProviderDto result(c);
        fill(result, parseRow(c, rows.front()));
        co_return result;
    }

  private:
    struct StoredProvider final {
        std::string id;
        std::string provider;
        std::int64_t revision{};
        std::string name;
        std::string accountId;
        std::string hint;
        std::string status;
        std::optional<std::string> lastVerifiedAt;
        std::optional<std::string> lastError;
        std::string createdAt;
        std::string updatedAt;
        std::int64_t zoneCount{};
    };

    static std::string selectColumns() {
        return "SELECT provider.id, provider.provider, provider.revision, provider.name, "
               "provider.account_id, provider.config::text, provider.status, "
               "TO_CHAR(provider.last_verified_at, 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF'), "
               "provider.last_error, TO_CHAR(provider.created_at, "
               "'YYYY-MM-DD\"T\"HH24:MI:SS.USOF'), "
               "TO_CHAR(provider.updated_at, 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF'), "
               "(SELECT COUNT(*) FROM sys_dns_zone zone WHERE zone.tenant_id = "
               "provider.tenant_id AND zone.provider_id = provider.id AND zone.deleted_at IS "
               "NULL)";
    }

    template <typename Row> static StoredProvider parseRow(ruvia::Context& c, const Row& row) {
        const auto config =
            service::dns::parseDnsProviderConfig(row[5].value().value_or("{}"), c.resource());
        return {
            .id = std::string(row[0].value().value_or("")),
            .provider = std::string(row[1].value().value_or("")),
            .revision = row[2].template as<std::int64_t>().value_or(0),
            .name = std::string(row[3].value().value_or("")),
            .accountId = std::string(row[4].value().value_or("")),
            .hint = config.credentialHint,
            .status = std::string(row[6].value().value_or("")),
            .lastVerifiedAt = optionalString(row[7]),
            .lastError = optionalString(row[8]),
            .createdAt = std::string(row[9].value().value_or("")),
            .updatedAt = std::string(row[10].value().value_or("")),
            .zoneCount = row[11].template as<std::int64_t>().value_or(0),
        };
    }

    template <typename Value> static std::optional<std::string> optionalString(const Value& value) {
        if (const auto result = value.value()) {
            return std::string(*result);
        }
        return std::nullopt;
    }

    static void fill(DnsProviderDto& item, const StoredProvider& provider) {
        item.set<"id">(provider.id);
        item.set<"revision">(provider.revision);
        item.set<"name">(provider.name);
        item.set<"accountId">(provider.accountId);
        item.set<"provider">(provider.provider);
        item.set<"tokenHint">(provider.hint);
        item.set<"status">(provider.status);
        item.set<"createdAt">(provider.createdAt);
        item.set<"updatedAt">(provider.updatedAt);
        item.set<"zoneCount">(provider.zoneCount);
        if (provider.lastVerifiedAt) {
            item.set<"lastVerifiedAt">(*provider.lastVerifiedAt);
        }
        if (provider.lastError) {
            item.set<"lastError">(*provider.lastError);
        }
    }
};

inline const DnsProviderReadService& dnsProviderReadService() {
    static const DnsProviderReadService service;
    return service;
}

} // namespace service::provider
