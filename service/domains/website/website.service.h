#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <ruvia/core/Task.h>
#include <ruvia/web/Context.h>
#include <ruvia/web/ModelJson.h>
#include <ruvia/web/db/DbTransaction.h>

#include "service/common/database.h"
#include "service/common/http.h"
#include "service/common/domain_name.h"
#include "service/domains/website/website.error.h"
#include "service/domains/website/website.types.h"
#include "service/features/log_ingest/tail.h"
#include "service/features/node_runtime/model.h"
#include "service/features/website_config/model.h"
#include "service/features/website_dns/model.h"
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
        std::string where =
            " FROM sys_website website INNER JOIN sys_cluster cluster ON cluster.tenant_id = "
            "website.tenant_id AND cluster.id = website.cluster_id INNER JOIN sys_dns_zone "
            "zone ON zone.tenant_id = cluster.tenant_id AND zone.id = cluster.dns_zone_id "
            "WHERE website.tenant_id = $1 AND website.deleted_at IS NULL AND "
            "cluster.deleted_at IS NULL AND zone.deleted_at IS NULL";
        std::vector<ruvia::DbValue> params{ruvia::DbValue{tenantId}};
        if (clusterId) {
            where += " AND website.cluster_id = $" + std::to_string(params.size() + 1);
            params.emplace_back(*clusterId);
        }
        if (status) {
            where += " AND website.status = $" + std::to_string(params.size() + 1);
            params.emplace_back(std::string_view(*status));
        }
        std::optional<std::string> keywordPattern;
        if (keyword) {
            keywordPattern = "%" + service::common::escapeLikePattern(*keyword) + "%";
            const auto placeholder = "$" + std::to_string(params.size() + 1);
            where += " AND (cluster.name ILIKE " + placeholder +
                     " OR EXISTS (SELECT 1 FROM sys_website_domain_claim claim WHERE "
                     "claim.tenant_id = website.tenant_id AND claim.website_id = "
                     "website.id AND claim.domain_key ILIKE " +
                     placeholder + "))";
            params.emplace_back(std::string_view(*keywordPattern));
        }

        const auto countRows = co_await c.db().query("SELECT COUNT(*)" + where, params);
        const auto total = countRows.empty() ? std::int64_t{0}
                                             : countRows.front()[0].as<std::int64_t>().value_or(0);
        const auto rows =
            co_await c.db().query(selectColumns() + where + " ORDER BY website.sort DESC LIMIT " +
                                      std::to_string(pageSize) + " OFFSET " + std::to_string(skip),
                                  params);
        std::vector<std::string> websiteIds;
        websiteIds.reserve(rows.size());
        for (const auto& row : rows) {
            websiteIds.emplace_back(row[0].value().value_or(""));
        }
        const auto certificates = co_await loadBoundCertificatesByWebsite(c, tenantId, websiteIds);

        WebsitePageDataDto result(c);
        auto& items = result.ensure<"list">();
        for (const auto& row : rows) {
            const auto config = parseConfig(c, row[5].value().value_or("{}"));
            const auto available = certificates.find(std::string(row[0].value().value_or("")));
            fillWebsite(c, items.emplace_back(c), row, config,
                        available == certificates.end() ? emptyCertificates() : available->second);
        }
        result.set<"total">(total);
        result.set<"page">(page);
        result.set<"pageSize">(pageSize);
        result.set<"totalPages">(pageSize > 0 ? (total + pageSize - 1) / pageSize : 0);
        co_return result;
    }

    ruvia::Task<WebsiteDto> detail(ruvia::Context& c, const std::string& tenantId,
                                   const std::string& id) {
        const auto rows = co_await c.db().query(
            selectColumns() +
                " FROM sys_website website INNER JOIN sys_cluster cluster ON "
                "cluster.tenant_id = website.tenant_id AND cluster.id = website.cluster_id "
                "INNER JOIN sys_dns_zone zone ON zone.tenant_id = cluster.tenant_id AND "
                "zone.id = cluster.dns_zone_id WHERE website.id = $1 AND website.tenant_id = "
                "$2 AND "
                "website.deleted_at IS NULL AND cluster.deleted_at IS NULL AND zone.deleted_at "
                "IS NULL LIMIT 1",
            id, tenantId);
        if (rows.empty()) {
            service::common::throwAppError(WebsiteError::NOT_FOUND);
        }
        const auto config = parseConfig(c, rows.front()[5].value().value_or("{}"));
        const auto certificates = co_await loadBoundCertificatesByWebsite(c, tenantId, {id});
        const auto originStates =
            co_await loadOriginRuntime(c, tenantId, rows.front()[1].value().value_or(""), id);
        WebsiteDto result(c);
        const auto available = certificates.find(id);
        fillWebsite(c, result, rows.front(), config,
                    available == certificates.end() ? emptyCertificates() : available->second,
                    originStates);
        co_return result;
    }

    ruvia::Task<WebsiteAccessLogTailDataDto>
    accessLogs(ruvia::Context& c, const std::string& tenantId, const std::string& id,
               std::int64_t limit, const std::optional<service::log_ingest::TailCursor>& after) {
        std::vector<ruvia::DbValue> params{ruvia::DbValue{tenantId}, ruvia::DbValue{id}};
        std::string cursorPredicate;
        if (after) {
            params.emplace_back(after->ingestedUnixMicros);
            params.emplace_back(std::string_view{after->id});
            cursorPredicate =
                " AND (access.created_at, access.id) > (TIMESTAMPTZ 'epoch' + $3::bigint * "
                "INTERVAL '1 microsecond', $4::uuid)";
        }
        params.emplace_back(limit);
        const auto limitParameter = "$" + std::to_string(params.size());
        const auto rows = co_await c.db().query(
            "SELECT log.id, TO_CHAR(log.occurred_at, 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF'), "
            "log.node_id, log.node_name, log.client_ip::text, log.protocol, log.method, log.host, "
            "log.target, log.status_code, log.response_bytes, log.duration_ms, log.user_agent, "
            "log.referer, log.request_headers, log.request_body, log.request_body_truncated, "
            "log.tls_fingerprint, log.response_headers, log.query_string, log.cookies, "
            "log.ingested_unix_micros FROM sys_website website LEFT JOIN LATERAL (SELECT "
            "access.*, COALESCE(node.name, '') AS node_name, "
            "ROUND(EXTRACT(EPOCH FROM access.created_at) * 1000000)::bigint AS "
            "ingested_unix_micros FROM sys_website_access_log access LEFT JOIN sys_node node ON "
            "node.tenant_id = access.tenant_id AND node.id = access.node_id WHERE "
            "access.tenant_id = website.tenant_id AND access.website_id = website.id" +
                cursorPredicate + " ORDER BY access.created_at DESC, access.id DESC LIMIT " +
                limitParameter +
                ") log ON TRUE WHERE website.tenant_id = $1 AND website.id = $2 AND "
                "website.deleted_at IS NULL ORDER BY log.created_at DESC NULLS LAST, log.id DESC",
            params);
        if (rows.empty()) {
            service::common::throwAppError(WebsiteError::NOT_FOUND);
        }

        WebsiteAccessLogTailDataDto result(c);
        auto& items = result.ensure<"list">();
        for (const auto& row : rows) {
            if (!row[0].value()) {
                continue;
            }
            auto& item = items.emplace_back(c);
            item.set<"id">(row[0].value().value_or(""));
            item.set<"occurredAt">(row[1].value().value_or(""));
            item.set<"nodeId">(row[2].value().value_or(""));
            item.set<"nodeName">(row[3].value().value_or(""));
            item.set<"protocol">(row[5].value().value_or(""));
            item.set<"method">(row[6].value().value_or(""));
            item.set<"host">(row[7].value().value_or(""));
            item.set<"target">(row[8].value().value_or(""));
            item.set<"statusCode">(row[9].as<std::int64_t>().value_or(0));
            item.set<"responseBytes">(row[10].as<std::int64_t>().value_or(0));
            item.set<"durationMs">(row[11].as<std::int64_t>().value_or(0));
            item.set<"requestBodyTruncated">(row[16].as<bool>().value_or(false));
            if (const auto value = row[4].value()) {
                item.set<"clientIp">(*value);
            }
            if (const auto value = row[12].value()) {
                item.set<"userAgent">(*value);
            }
            if (const auto value = row[13].value()) {
                item.set<"referer">(*value);
            }
            if (const auto value = row[14].value()) {
                item.set<"requestHeaders">(*value);
            }
            if (const auto value = row[15].value()) {
                item.set<"requestBody">(*value);
            }
            if (const auto value = row[17].value()) {
                item.set<"tlsFingerprint">(*value);
            }
            if (const auto value = row[18].value()) {
                item.set<"responseHeaders">(*value);
            }
            if (const auto value = row[19].value()) {
                item.set<"queryString">(*value);
            }
            if (const auto value = row[20].value()) {
                item.set<"cookies">(*value);
            }
        }
        if (!items.empty()) {
            result.set<"cursor">(service::log_ingest::encodeTailCursor(
                rows.front()[21].as<std::int64_t>().value_or(0),
                rows.front()[0].value().value_or("")));
        }
        co_return result;
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
            (void)co_await service::sync_runtime::upsertMarker(transaction, tenantId, "website",
                                                               websiteId, "apply", 1);
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
            (void)co_await service::sync_runtime::upsertMarker(transaction, tenantId, "website", id,
                                                               "apply", revision);
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
        co_await service::sync_runtime::removeMarker(transaction, tenantId, "website", id);
        co_await service::website_dns::reconcileConfigChange(transaction, tenantId, previousConfig,
                                                             std::nullopt);
        co_await transaction.commit();
        co_return;
    }

  private:
    struct BoundCertificate {
        std::string id;
        std::vector<std::string> domains;
        bool usable;
    };

    struct DomainClaim {
        std::string id;
        std::string key;
        std::string dnsMode;
        std::optional<std::string> dnsZoneId{};
    };

    struct OriginRuntimeState {
        std::string nodeId;
        std::string nodeName;
        service::node_runtime::NodeRuntimeData::OriginHealth health;
    };

    static std::string selectColumns() {
        return "SELECT website.id, website.cluster_id, cluster.name, "
               "cluster.hostname_prefix || '.' || zone.domain, website.revision, "
               "website.config::text, website.runtime::text, (SELECT COUNT(*) FROM sys_node "
               "node WHERE node.tenant_id = website.tenant_id AND node.cluster_id = "
               "website.cluster_id AND node.deleted_at IS NULL AND "
               "node.status = 'enabled' AND node.registration_status = 'registered'), (SELECT "
               "COUNT(*) FROM sys_node node WHERE node.tenant_id = website.tenant_id AND "
               "node.cluster_id = website.cluster_id AND node.deleted_at IS NULL AND node.status "
               "= 'enabled' AND node.registration_status = 'registered' AND "
               "node.desired_release_id IS NOT NULL AND node.active_release_id = "
               "node.desired_release_id), TO_CHAR(website.created_at, "
               "'YYYY-MM-DD\"T\"HH24:MI:SS.USOF'), TO_CHAR(website.updated_at, "
               "'YYYY-MM-DD\"T\"HH24:MI:SS.USOF'), website.status";
    }

    static std::string serializeConfig(ruvia::Context& c,
                                       const service::website_config::WebsiteConfigData& input) {
        const auto output = service::website_config::toOutput(input, {.resource = c.resource()});
        const auto json = ruvia::toJson(output, {.resource = c.resource()});
        return std::string(json.data(), json.size());
    }

    static service::website_config::WebsiteConfigData parseConfig(ruvia::Context& c,
                                                                  std::string_view json) {
        auto config = service::website_config::parseStored(json, {.resource = c.resource()});
        if (!config) {
            throwCorruptConfig();
        }
        return std::move(*config);
    }

    using BoundCertificatesByWebsite =
        std::unordered_map<std::string, std::vector<BoundCertificate>>;

    static const std::vector<BoundCertificate>& emptyCertificates() {
        static const std::vector<BoundCertificate> value;
        return value;
    }

    static ruvia::Task<BoundCertificatesByWebsite>
    loadBoundCertificatesByWebsite(ruvia::Context& c, const std::string& tenantId,
                                   const std::vector<std::string>& websiteIds) {
        BoundCertificatesByWebsite result;
        if (websiteIds.empty()) {
            co_return result;
        }
        std::vector<ruvia::DbValue> params{ruvia::DbValue{tenantId}};
        params.reserve(1 + websiteIds.size());
        std::string placeholders;
        for (const auto& websiteId : websiteIds) {
            if (!placeholders.empty()) {
                placeholders += ", ";
            }
            placeholders += "$" + std::to_string(params.size() + 1);
            params.emplace_back(websiteId);
        }
        const auto rows = co_await c.db().query(
            "SELECT binding.website_id, certificate.id, certificate.subject_alt_names[1], "
            "certificate.subject_alt_names[2], COALESCE(certificate.issued_revision > 0 AND "
            "certificate.expires_at > NOW(), FALSE) FROM "
            "sys_website_certificate_binding binding INNER JOIN sys_certificate certificate ON "
            "certificate.tenant_id = binding.tenant_id AND certificate.id = "
            "binding.certificate_id WHERE binding.tenant_id = $1 AND binding.website_id IN (" +
                placeholders +
                ") AND certificate.deleted_at IS NULL ORDER BY binding.website_id, "
                "binding.position",
            params);
        for (const auto& row : rows) {
            std::vector<std::string> domains;
            if (const auto value = row[2].value()) {
                domains.emplace_back(*value);
            }
            if (const auto value = row[3].value()) {
                domains.emplace_back(*value);
            }
            result[std::string(row[0].value().value_or(""))].push_back(
                {.id = std::string(row[1].value().value_or("")),
                 .domains = std::move(domains),
                 .usable = row[4].as<bool>().value_or(false)});
        }
        co_return result;
    }

    static ruvia::Task<std::vector<OriginRuntimeState>>
    loadOriginRuntime(ruvia::Context& c, const std::string& tenantId, std::string_view clusterId,
                      std::string_view websiteId) {
        const auto rows = co_await c.db().query(
            "SELECT id, name, runtime::text FROM sys_node WHERE tenant_id = $1 AND cluster_id = "
            "$2 AND status = 'enabled' AND registration_status = 'registered' AND "
            "deleted_at IS NULL ORDER BY sort ASC",
            tenantId, clusterId);
        std::vector<OriginRuntimeState> result;
        for (const auto& row : rows) {
            const auto runtime = service::node_runtime::parseStored(row[2].value().value_or("{}"),
                                                                    {.resource = c.resource()});
            if (!runtime) {
                continue;
            }
            for (const auto& health : runtime->originHealth) {
                if (health.websiteId == websiteId) {
                    result.push_back({.nodeId = std::string(row[0].value().value_or("")),
                                      .nodeName = std::string(row[1].value().value_or("")),
                                      .health = health});
                }
            }
        }
        co_return result;
    }

    template <typename Row>
    static void fillWebsite(ruvia::Context& c, WebsiteDto& item, const Row& row,
                            const service::website_config::WebsiteConfigData& config,
                            const std::vector<BoundCertificate>& certificates,
                            const std::vector<OriginRuntimeState>& originStates = {}) {
        const auto runtime = service::website_dns::parseStored(row[6].value().value_or("{}"),
                                                               {.resource = c.resource()});
        if (!runtime) {
            throwCorruptConfig();
        }
        const auto targetCount = row[7].template as<std::int64_t>().value_or(0);
        const auto syncedCount = row[8].template as<std::int64_t>().value_or(0);
        item.set<"id">(row[0].value().value_or(""));
        item.set<"clusterId">(row[1].value().value_or(""));
        item.set<"clusterName">(row[2].value().value_or(""));
        item.set<"accessDomain">(row[3].value().value_or(""));
        item.set<"status">(row[11].value().value_or(""));
        item.set<"revision">(row[4].template as<std::int64_t>().value_or(1));
        item.set<"config">(service::website_config::toOutput(config, {.resource = c.resource()}));
        auto& certificateDtos = item.ensure<"certificates">();
        for (const auto& certificate : certificates) {
            auto& output = certificateDtos.emplace_back(c);
            output.set<"id">(certificate.id);
            output.set<"usable">(certificate.usable);
            auto& domains = output.ensure<"domains">();
            for (const auto& domain : certificate.domains) {
                domains.emplace_back(domain, ruvia::ModelOptions{.resource = c.resource()});
            }
        }
        item.set<"runtime">(
            toRuntime(c, config, *runtime, certificates, targetCount, syncedCount, originStates));
        item.set<"createdAt">(row[9].value().value_or(""));
        item.set<"updatedAt">(row[10].value().value_or(""));
    }

    static WebsiteRuntimeDto toRuntime(ruvia::Context& c,
                                       const service::website_config::WebsiteConfigData& config,
                                       const service::website_dns::WebsiteRuntimeData& input,
                                       const std::vector<BoundCertificate>& certificates,
                                       std::int64_t targetCount, std::int64_t syncedCount,
                                       const std::vector<OriginRuntimeState>& originStates = {}) {
        WebsiteRuntimeDto output(c);
        const auto deployStatus = targetCount == 0             ? std::string_view{"no_nodes"}
                                  : syncedCount == targetCount ? std::string_view{"applied"}
                                  : syncedCount == 0           ? std::string_view{"pending"}
                                                               : std::string_view{"partial"};
        output.set<"deployStatus">(deployStatus);
        output.set<"targetNodeCount">(targetCount);
        output.set<"syncedNodeCount">(syncedCount);
        auto& domains = output.ensure<"domainStates">();
        for (const auto& domain : config.domains) {
            const service::website_dns::WebsiteDomainRuntimeData* runtimeState = nullptr;
            const auto state = std::ranges::find_if(input.domainStates, [&](const auto& candidate) {
                return candidate.id && *candidate.id == domain.id;
            });
            if (state != input.domainStates.end()) {
                runtimeState = &*state;
            }
            const bool https =
                config.httpsEnabled &&
                std::ranges::any_of(certificates, [&](const auto& certificate) {
                    return certificate.usable &&
                           std::ranges::any_of(
                               certificate.domains, [&](const auto& certificateDomain) {
                                   return service::common::certificateCoversHostname(
                                       certificateDomain, domain.hostname);
                               });
                });
            auto& item = domains.emplace_back(c);
            std::string_view resolutionStatus{"unverified"};
            if (runtimeState && runtimeState->resolutionStatus) {
                resolutionStatus = *runtimeState->resolutionStatus;
            }
            item.set<"id">(domain.id);
            item.set<"accessProtocol">(https ? std::string_view{"https"}
                                             : std::string_view{"http"});
            item.set<"resolutionStatus">(resolutionStatus);
            if (runtimeState) {
                if (runtimeState->lastVerifiedAt) {
                    item.set<"lastVerifiedAt">(*runtimeState->lastVerifiedAt);
                }
                if (runtimeState->lastError) {
                    item.set<"lastError">(*runtimeState->lastError);
                }
            }
        }
        auto& origins = output.ensure<"originStates">();
        origins.reserve(originStates.size());
        for (const auto& state : originStates) {
            auto& item = origins.emplace_back(c);
            item.set<"nodeId">(state.nodeId);
            item.set<"nodeName">(state.nodeName);
            item.set<"originId">(state.health.originId);
            item.set<"status">(state.health.status);
            item.set<"checkedAtUnixMillis">(state.health.checkedAtUnixMillis);
            item.set<"latencyMillis">(state.health.latencyMillis);
            if (state.health.lastError) {
                item.set<"lastError">(*state.health.lastError);
            }
        }
        return output;
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
