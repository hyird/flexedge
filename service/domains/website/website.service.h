#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ruvia/core/Task.h>
#include <ruvia/web/Context.h>
#include <ruvia/web/ModelJson.h>
#include <ruvia/web/db/DbTransaction.h>

#include "service/common/database.h"
#include "service/common/http.h"
#include "service/common/domain_name.h"
#include "service/domains/website/website_access_log.service.h"
#include "service/domains/website/website.error.h"
#include "service/domains/website/website.types.h"
#include "service/domains/website/website_dashboard.service.h"
#include "service/domains/website/website_read.service.h"
#include "service/features/log_ingest/tail.h"
#include "service/features/website_config/model.h"
#include "service/features/website_dns/projection.h"
#include "service/features/node_dispatch/queue.h"
#include "service/features/sync_runtime/state.h"

namespace service::website {

class WebsiteService {
  public:
    ruvia::Task<WebsitePageDataDto>
    list(ruvia::Context& c, const std::string& tenantId, std::int64_t page, std::int64_t pageSize,
         std::int64_t skip, const std::optional<std::string>& keyword,
         const std::optional<std::string>& clusterId, const std::optional<std::string>& status) {
        co_return co_await websiteReadService().list(c, tenantId, page, pageSize, skip, keyword,
                                                     clusterId, status);
    }

    ruvia::Task<WebsiteDto> detail(ruvia::Context& c, const std::string& tenantId,
                                   const std::string& id) {
        co_return co_await websiteReadService().detail(c, tenantId, id);
    }

    ruvia::Task<WebsiteAccessLogTailDataDto>
    accessLogs(ruvia::Context& c, const std::string& tenantId, const std::string& id,
               std::int64_t limit, const std::optional<service::log_ingest::TailCursor>& after) {
        co_return co_await websiteAccessLogService().tail(c, tenantId, id, limit, after);
    }

    ruvia::Task<WebsiteAccessLogPageDataDto> accessLogHistory(
        ruvia::Context& c, const std::string& tenantId, const std::string& id, std::int64_t page,
        std::int64_t pageSize, std::int64_t skip, const std::optional<std::string>& keyword,
        const std::optional<std::string>& method, const std::optional<std::string>& statusClass) {
        co_return co_await websiteAccessLogService().history(c, tenantId, id, page, pageSize, skip,
                                                             keyword, method, statusClass);
    }

    ruvia::Task<WebsiteDashboardDto> dashboard(ruvia::Context& c, const std::string& tenantId,
                                               const std::string& id) {
        co_return co_await websiteDashboardService().dashboard(c, tenantId, id);
    }

    ruvia::Task<void> create(ruvia::Context& c, const std::string& tenantId,
                             const std::string& clusterId,
                             const ruvia::ValidatedJson<WebsiteSaveInput>& body) {
        const auto normalized = normalize(body.value());
        if (!normalized) {
            throwCorruptConfig();
        }
        const auto& status = normalized->status;
        const auto& config = normalized->config;
        const auto configJson = serializeConfig(c, config);
        try {
            auto transaction = co_await c.db().beginTransaction();
            co_await requireCluster(transaction, tenantId, clusterId);
            const auto domainClaims =
                co_await buildDomainClaims(transaction, tenantId, std::nullopt, config);
            co_await validateCertificates(transaction, tenantId, config);
            const auto rows = co_await transaction.query(
                "INSERT INTO sys_website (tenant_id, cluster_id, status, revision, config, "
                "runtime, "
                "created_at, updated_at) VALUES ($1, $2, $3, 1, $4::jsonb, '{}'::jsonb, NOW(), "
                "NOW()) RETURNING id",
                service::common::dbParams(ruvia::DbValue{tenantId}, ruvia::DbValue{clusterId},
                                          ruvia::DbValue{status},
                                          ruvia::DbValue{std::string_view(configJson)}));
            const auto websiteId = std::string(rows.front()[0].value().value_or(""));
            co_await replaceRelationProjections(transaction, tenantId, websiteId, domainClaims,
                                                config);
            co_await service::node_dispatch::publishClusterRelease(transaction, tenantId,
                                                                   clusterId);
            (void)co_await service::sync_runtime::upsertMarker(
                transaction, tenantId, service::sync_runtime::MarkerResourceType::website,
                websiteId, service::sync_runtime::MarkerOperation::apply, 1);
            co_await service::website_dns::reconcileConfigChange(transaction, tenantId,
                                                                 std::nullopt, configJson);
            co_await transaction.commit();
        } catch (const ruvia::DbError& error) {
            if (isDomainClaimConflict(error)) {
                service::common::throwAppError(WebsiteError::DOMAIN_EXISTS);
            }
            throw;
        }
        co_return;
    }

    ruvia::Task<void> update(ruvia::Context& c, const std::string& tenantId, const std::string& id,
                             const std::string& clusterId, std::int64_t expectedRevision,
                             const ruvia::ValidatedJson<WebsiteSaveInput>& body) {
        const auto normalized = normalize(body.value());
        if (!normalized) {
            throwCorruptConfig();
        }
        const auto& status = normalized->status;
        const auto& config = normalized->config;
        const auto configJson = serializeConfig(c, config);
        try {
            auto transaction = co_await c.db().beginTransaction();
            const auto previousRows = co_await transaction.query(
                "SELECT config::text, cluster_id FROM sys_website WHERE id = $1 AND tenant_id "
                "= $2 AND "
                "deleted_at IS NULL LIMIT 1 FOR UPDATE",
                id, tenantId);
            if (previousRows.empty()) {
                service::common::throwAppError(WebsiteError::NOT_FOUND);
            }
            co_await requireCluster(transaction, tenantId, clusterId);
            const auto domainClaims = co_await buildDomainClaims(transaction, tenantId, id, config);
            co_await validateCertificates(transaction, tenantId, config);
            const auto previousConfig = std::string(previousRows.front()[0].value().value_or("{}"));
            const auto previousClusterId =
                std::string(previousRows.front()[1].value().value_or(""));
            const auto rows = co_await transaction.query(
                "UPDATE sys_website SET cluster_id = $1, status = $2, config = $3::jsonb, "
                "revision = revision + 1, updated_at = NOW() WHERE id = $4 AND tenant_id = $5 "
                "AND revision = $6 AND deleted_at IS NULL RETURNING revision",
                service::common::dbParams(ruvia::DbValue{clusterId}, ruvia::DbValue{status},
                                          ruvia::DbValue{std::string_view(configJson)},
                                          ruvia::DbValue{id}, ruvia::DbValue{tenantId},
                                          ruvia::DbValue{expectedRevision}));
            if (rows.empty()) {
                const auto current = co_await transaction.query(
                    "SELECT revision FROM sys_website WHERE id = $1 AND tenant_id = $2 AND "
                    "deleted_at IS NULL",
                    id, tenantId);
                if (current.empty()) {
                    service::common::throwAppError(WebsiteError::NOT_FOUND);
                }
                service::common::throwAppError(WebsiteError::REVISION_CONFLICT);
            }
            const auto revision = rows.front()[0].as<std::int64_t>().value_or(expectedRevision + 1);
            co_await replaceRelationProjections(transaction, tenantId, id, domainClaims, config);
            if (previousClusterId != clusterId) {
                co_await service::node_dispatch::publishClusterRelease(transaction, tenantId,
                                                                       previousClusterId);
            }
            co_await service::node_dispatch::publishClusterRelease(transaction, tenantId,
                                                                   clusterId);
            (void)co_await service::sync_runtime::upsertMarker(
                transaction, tenantId, service::sync_runtime::MarkerResourceType::website, id,
                service::sync_runtime::MarkerOperation::apply, revision);
            co_await service::website_dns::reconcileConfigChange(transaction, tenantId,
                                                                 previousConfig, configJson);
            co_await transaction.commit();
        } catch (const ruvia::DbError& error) {
            if (isDomainClaimConflict(error)) {
                service::common::throwAppError(WebsiteError::DOMAIN_EXISTS);
            }
            throw;
        }
        co_return;
    }

    ruvia::Task<void> remove(ruvia::Context& c, const std::string& tenantId, const std::string& id,
                             std::int64_t expectedRevision) {
        auto transaction = co_await c.db().beginTransaction();
        const auto rows = co_await transaction.query(
            "UPDATE sys_website SET deleted_at = NOW(), revision = revision + 1, updated_at = "
            "NOW() WHERE id = $1 AND tenant_id = $2 AND revision = $3 AND deleted_at IS NULL "
            "RETURNING revision, config::text, cluster_id, status",
            id, tenantId, expectedRevision);
        if (rows.empty()) {
            const auto current = co_await transaction.query(
                "SELECT revision FROM sys_website WHERE id = $1 AND tenant_id = $2 AND "
                "deleted_at IS NULL",
                id, tenantId);
            if (current.empty()) {
                service::common::throwAppError(WebsiteError::NOT_FOUND);
            }
            service::common::throwAppError(WebsiteError::REVISION_CONFLICT);
        }
        const auto previousConfig = std::string(rows.front()[1].value().value_or("{}"));
        const auto clusterId = std::string(rows.front()[2].value().value_or(""));
        (void)co_await transaction.execute(
            "DELETE FROM sys_website_certificate_binding WHERE tenant_id = $1 AND website_id "
            "= $2",
            tenantId, id);
        (void)co_await transaction.execute(
            "DELETE FROM sys_website_domain_claim WHERE tenant_id = $1 AND website_id = $2",
            tenantId, id);
        co_await service::node_dispatch::publishClusterRelease(transaction, tenantId, clusterId);
        co_await service::sync_runtime::removeMarker(
            transaction, tenantId, service::sync_runtime::MarkerResourceType::website, id);
        co_await service::website_dns::reconcileConfigChange(transaction, tenantId, previousConfig,
                                                             std::nullopt);
        co_await transaction.commit();
        co_return;
    }

  private:
    struct DomainClaim {
        std::string id;
        std::string key;
        std::string dnsMode;
        std::optional<std::string> dnsZoneId{};
    };

    static std::string serializeConfig(ruvia::Context& c,
                                       const service::website_config::WebsiteConfigData& input) {
        const auto output = service::website_config::toOutput(input, {.resource = c.resource()});
        const auto json = ruvia::toJson(output, {.resource = c.resource()});
        return std::string(json.data(), json.size());
    }

    template <typename Db>
    static ruvia::Task<void> requireCluster(Db& db, const std::string& tenantId,
                                            const std::string& clusterId) {
        const auto rows = co_await db.query(
            "SELECT id FROM sys_cluster WHERE id = $1 AND tenant_id = $2 AND status = "
            "'enabled' AND deleted_at IS NULL LIMIT 1 FOR SHARE",
            clusterId, tenantId);
        if (rows.empty()) {
            service::common::throwAppError(WebsiteError::CLUSTER_UNAVAILABLE);
        }
        co_return;
    }

    template <typename Db>
    static ruvia::Task<std::vector<DomainClaim>>
    buildDomainClaims(Db& db, const std::string& tenantId,
                      const std::optional<std::string>& excludedWebsiteId,
                      const service::website_config::WebsiteConfigData& config) {
        const auto zones = co_await db.query(
            "SELECT id, domain FROM sys_dns_zone WHERE tenant_id = $1 AND deleted_at IS NULL "
            "ORDER BY length(domain) DESC, sort ASC FOR SHARE",
            tenantId);
        std::vector<DomainClaim> claims;
        claims.reserve(config.domains.size());
        for (const auto& domain : config.domains) {
            const auto key = lower(domain.hostname);
            DomainClaim claim{.id = domain.id, .key = key, .dnsMode = domain.dnsMode};
            if (claim.dnsMode == "managed") {
                const auto owner = std::ranges::find_if(zones, [&](const auto& zone) {
                    return service::common::domainBelongsToZone(domain.hostname,
                                                                zone[1].value().value_or(""));
                });
                if (owner == zones.end()) {
                    service::common::throwAppError(WebsiteError::MANAGED_ZONE_NOT_FOUND);
                }
                auto hostname = std::string_view{domain.hostname};
                if (hostname.starts_with("*.")) {
                    hostname.remove_prefix(2);
                }
                if (service::common::normalizeDomainName(hostname) ==
                    service::common::normalizeDomainName((*owner)[1].value().value_or(""))) {
                    service::common::throwAppError(WebsiteError::MANAGED_ZONE_APEX_UNSUPPORTED);
                }
                claim.dnsZoneId = std::string((*owner)[0].value().value_or(""));
            }
            claims.push_back(std::move(claim));
        }

        std::vector<ruvia::DbValue> params{ruvia::DbValue{tenantId}};
        params.reserve(1 + claims.size() + (excludedWebsiteId ? 1 : 0));
        std::string placeholders;
        for (const auto& claim : claims) {
            if (!placeholders.empty()) {
                placeholders += ", ";
            }
            placeholders += "$" + std::to_string(params.size() + 1);
            params.emplace_back(std::string_view{claim.key});
        }
        std::string duplicateSql =
            "SELECT 1 FROM sys_website_domain_claim WHERE tenant_id = $1 AND domain_key IN (" +
            placeholders + ")";
        if (excludedWebsiteId) {
            duplicateSql += " AND website_id <> $" + std::to_string(params.size() + 1);
            params.emplace_back(std::string_view{*excludedWebsiteId});
        }
        duplicateSql += " LIMIT 1";
        const auto duplicateRows = co_await db.query(duplicateSql, params);
        if (!duplicateRows.empty()) {
            service::common::throwAppError(WebsiteError::DOMAIN_EXISTS);
        }
        co_return claims;
    }

    template <typename Db>
    static ruvia::Task<void>
    validateCertificates(Db& db, const std::string& tenantId,
                         const service::website_config::WebsiteConfigData& config) {
        const auto httpsEnabled = config.httpsEnabled;
        const auto& certificateIds = config.certificateIds;
        if ((httpsEnabled && certificateIds.empty()) ||
            (!httpsEnabled && !certificateIds.empty())) {
            service::common::throwAppError(WebsiteError::HTTPS_CERTIFICATE_SELECTION_INVALID);
        }
        if (certificateIds.empty()) {
            co_return;
        }

        std::vector<ruvia::DbValue> params{ruvia::DbValue{tenantId}};
        params.reserve(1 + certificateIds.size());
        std::string placeholders;
        for (const auto& certificateId : certificateIds) {
            if (!placeholders.empty()) {
                placeholders += ", ";
            }
            placeholders += "$" + std::to_string(params.size() + 1);
            params.emplace_back(certificateId);
        }
        std::string sql =
            "SELECT certificate.id, certificate.domain FROM sys_certificate certificate WHERE "
            "certificate.tenant_id = $1 AND certificate.id IN (" +
            placeholders + ") AND certificate.deleted_at IS NULL";
        sql += " AND certificate.issued_revision > 0 AND certificate.expires_at > NOW()";
        sql += " FOR SHARE";
        const auto rows = co_await db.query(sql, params);
        if (rows.size() != certificateIds.size()) {
            service::common::throwAppError(WebsiteError::CERTIFICATE_UNAVAILABLE);
        }
        co_return;
    }

    static ruvia::Task<void>
    replaceRelationProjections(ruvia::DbTransaction& transaction, const std::string& tenantId,
                               const std::string& websiteId,
                               const std::vector<DomainClaim>& domainClaims,
                               const service::website_config::WebsiteConfigData& config) {
        (void)co_await transaction.execute(
            "DELETE FROM sys_website_certificate_binding WHERE tenant_id = $1 AND website_id "
            "= $2",
            tenantId, websiteId);
        (void)co_await transaction.execute(
            "DELETE FROM sys_website_domain_claim WHERE tenant_id = $1 AND website_id = $2",
            tenantId, websiteId);

        if (!domainClaims.empty()) {
            std::vector<ruvia::DbValue> domainInsertParams;
            domainInsertParams.reserve(domainClaims.size() * 6);
            std::string domainInsertSql =
                "INSERT INTO sys_website_domain_claim (tenant_id, website_id, domain_id, "
                "domain_key, dns_mode, dns_zone_id) VALUES ";
            const auto appendParam = [&](ruvia::DbValue value) {
                domainInsertSql += "$" + std::to_string(domainInsertParams.size() + 1);
                domainInsertParams.push_back(std::move(value));
            };
            for (const auto& claim : domainClaims) {
                if (!domainInsertParams.empty()) {
                    domainInsertSql += ", ";
                }
                domainInsertSql += "(";
                appendParam(ruvia::DbValue{tenantId});
                domainInsertSql += ", ";
                appendParam(ruvia::DbValue{websiteId});
                domainInsertSql += ", ";
                appendParam(ruvia::DbValue{std::string_view{claim.id}});
                domainInsertSql += ", ";
                appendParam(ruvia::DbValue{std::string_view{claim.key}});
                domainInsertSql += ", ";
                appendParam(ruvia::DbValue{std::string_view{claim.dnsMode}});
                domainInsertSql += ", ";
                appendParam(claim.dnsZoneId ? ruvia::DbValue{std::string_view{*claim.dnsZoneId}}
                                            : ruvia::DbValue{nullptr});
                domainInsertSql += ")";
            }
            (void)co_await transaction.execute(domainInsertSql, domainInsertParams);
        }

        const auto& certificateIds = config.certificateIds;
        if (!certificateIds.empty()) {
            std::vector<ruvia::DbValue> certificateInsertParams;
            certificateInsertParams.reserve(certificateIds.size() * 4);
            std::string certificateInsertSql =
                "INSERT INTO sys_website_certificate_binding (tenant_id, website_id, "
                "certificate_id, position) VALUES ";
            const auto appendParam = [&](ruvia::DbValue value) {
                certificateInsertSql += "$" + std::to_string(certificateInsertParams.size() + 1);
                certificateInsertParams.push_back(std::move(value));
            };
            for (std::size_t position = 0; position < certificateIds.size(); ++position) {
                if (position > 0) {
                    certificateInsertSql += ", ";
                }
                certificateInsertSql += "(";
                appendParam(ruvia::DbValue{tenantId});
                certificateInsertSql += ", ";
                appendParam(ruvia::DbValue{websiteId});
                certificateInsertSql += ", ";
                appendParam(ruvia::DbValue{certificateIds[position]});
                certificateInsertSql += ", ";
                appendParam(ruvia::DbValue{static_cast<std::int64_t>(position)});
                certificateInsertSql += ")";
            }
            (void)co_await transaction.execute(certificateInsertSql, certificateInsertParams);
        }
        co_return;
    }

    static std::string lower(std::string_view input) {
        std::string result(input);
        std::ranges::transform(result, result.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return result;
    }

    static bool isDomainClaimConflict(const ruvia::DbError& error) {
        return service::common::isUniqueConstraintViolation(error, "uq_website_domain_claim");
    }

    [[noreturn]] static void throwCorruptConfig() {
        service::common::throwAppError(service::common::kServerErrorCode, "聚合配置损坏", 500);
    }
};

inline WebsiteService& websiteService() {
    static WebsiteService service;
    return service;
}

} // namespace service::website
