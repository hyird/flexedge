#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <ruvia/core/Task.h>
#include <ruvia/web/Context.h>
#include <ruvia/web/db/Db.h>

#include "service/common/http.h"
#include "service/domains/website/website.error.h"
#include "service/domains/website/website.types.h"
#include "service/domains/website/website_runtime.mapper.h"
#include "service/features/node_runtime/mapper.h"
#include "service/features/website_config/mapper.h"
#include "service/features/website_dns/mapper.h"

namespace service::website {

class WebsiteReadService final {
  public:
    ruvia::Task<WebsitePageDataDto> list(auto& c, const std::string& tenantId,
                                         std::int64_t page, std::int64_t pageSize,
                                         std::int64_t skip,
                                         const std::optional<std::string>& keyword,
                                         const std::optional<std::string>& clusterId,
                                         const std::optional<std::string>& status) const {
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
                                             : countRows.front()[0].template as<std::int64_t>().value_or(0);
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
        auto& items = result.template ensure<"list">();
        for (const auto& row : rows) {
            const auto config = parseConfig(c, row[5].value().value_or("{}"));
            const auto available = certificates.find(std::string(row[0].value().value_or("")));
            fillWebsite(c, items.emplace_back(c), row, config,
                        available == certificates.end() ? emptyCertificates() : available->second);
        }
        result.template set<"total">(total);
        result.template set<"page">(page);
        result.template set<"pageSize">(pageSize);
        result.template set<"totalPages">(pageSize > 0 ? (total + pageSize - 1) / pageSize : 0);
        co_return result;
    }

    ruvia::Task<WebsiteDto> detail(auto& c, const std::string& tenantId,
                                   const std::string& id) const {
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
        ruvia::Array<WebsiteOriginSourceDto> sources(c.resource());
        const auto originStates =
            co_await loadOriginRuntime(c, tenantId, rows.front()[1].value().value_or(""), id, sources);
        WebsiteDto result(c);
        const auto available = certificates.find(id);
        fillWebsite(c, result, rows.front(), config,
                    available == certificates.end() ? emptyCertificates() : available->second,
                    originStates);
        result.template ensure<"runtime">().template set<"originSources">(std::move(sources));
        co_return result;
    }

  private:
    using BoundCertificate = detail::BoundCertificate;
    using BoundCertificatesByWebsite =
        std::unordered_map<std::string, std::vector<BoundCertificate>>;
    using OriginRuntimeState = detail::OriginRuntimeState;

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

    static service::website_config::WebsiteConfigData parseConfig(auto& c,
                                                                  std::string_view json) {
        auto config = service::website_config::parseStored(json, {.resource = c.resource()});
        if (!config) {
            throwCorruptConfig();
        }
        return std::move(*config);
    }

    static const std::vector<BoundCertificate>& emptyCertificates() {
        static const std::vector<BoundCertificate> value;
        return value;
    }

    static ruvia::Task<BoundCertificatesByWebsite>
    loadBoundCertificatesByWebsite(auto& c, const std::string& tenantId,
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
                 .usable = row[4].template as<bool>().value_or(false)});
        }
        co_return result;
    }

    static ruvia::Task<std::vector<OriginRuntimeState>>
    loadOriginRuntime(auto& c, const std::string& tenantId, std::string_view clusterId,
                      std::string_view websiteId, ruvia::Array<WebsiteOriginSourceDto>& sources) {
        const auto rows = co_await c.db().query(
            "SELECT id, name, runtime::text, revision, "
            "TO_CHAR(last_heartbeat_at, 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF') "
            "FROM sys_node WHERE tenant_id = $1 AND cluster_id = "
            "$2 AND status = 'enabled' AND registration_status = 'registered' AND "
            "deleted_at IS NULL ORDER BY sort ASC",
            tenantId, clusterId);
        std::vector<OriginRuntimeState> result;
        for (const auto& row : rows) {
            auto& source = sources.emplace_back(c);
            source.template set<"nodeId">(row[0].value().value_or(""));
            source.template set<"nodeRevision">(row[3].template as<std::int64_t>().value_or(0));
            source.template set<"reportedAt">(row[4].value().value_or(""));
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
    static void fillWebsite(auto& c, WebsiteDto& item, const Row& row,
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
        item.template set<"id">(row[0].value().value_or(""));
        item.template set<"clusterId">(row[1].value().value_or(""));
        item.template set<"clusterName">(row[2].value().value_or(""));
        item.template set<"accessDomain">(row[3].value().value_or(""));
        item.template set<"status">(row[11].value().value_or(""));
        item.template set<"revision">(row[4].template as<std::int64_t>().value_or(1));
        item.template set<"config">(service::website_config::toOutput(config, {.resource = c.resource()}));
        auto& certificateDtos = item.template ensure<"certificates">();
        for (const auto& certificate : certificates) {
            auto& output = certificateDtos.emplace_back(c);
            output.template set<"id">(certificate.id);
            output.template set<"usable">(certificate.usable);
            auto& domains = output.template ensure<"domains">();
            for (const auto& domain : certificate.domains) {
                domains.emplace_back(domain, ruvia::ModelOptions{.resource = c.resource()});
            }
        }
        item.template set<"runtime">(detail::toRuntime(c, config, *runtime, certificates, targetCount,
                                              syncedCount, originStates));
        item.template set<"createdAt">(row[9].value().value_or(""));
        item.template set<"updatedAt">(row[10].value().value_or(""));
    }

    [[noreturn]] static void throwCorruptConfig() {
        service::common::throwAppError(service::common::kServerErrorCode, "聚合配置损坏", 500);
    }
};

inline const WebsiteReadService& websiteReadService() {
    static const WebsiteReadService service;
    return service;
}

} // namespace service::website
