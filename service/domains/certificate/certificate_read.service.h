#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ruvia/core/Task.h>
#include <ruvia/web/Context.h>
#include <ruvia/web/db/Db.h>

#include "service/common/http.h"
#include "service/domains/certificate/certificate.error.h"
#include "service/domains/certificate/certificate.mapper.h"
#include "service/domains/certificate/certificate.types.h"
#include "service/features/certificate_material/archive.h"
#include "service/features/certificate_material/download.h"
#include "service/features/certificate_material/model.h"
#include "service/utils/secret.h"
#include "service/utils/sensitive_string.h"

namespace service::certificate {

class CertificateReadService final {
  public:
    ruvia::Task<CertificatePageDataDto>
    list(ruvia::Context& c, const std::string& tenantId, std::int64_t page, std::int64_t pageSize,
         std::int64_t skip, const std::optional<std::string>& keyword,
         std::optional<std::string_view> status, std::optional<bool> usable) const {
        std::string where =
            " FROM sys_certificate cert INNER JOIN sys_dns_zone zone ON zone.tenant_id = "
            "cert.tenant_id AND zone.id = cert.dns_zone_id INNER JOIN sys_provider provider ON "
            "provider.tenant_id = cert.tenant_id AND provider.id = cert.provider_id AND "
            "provider.kind = 'certificate' LEFT JOIN LATERAL (SELECT task.operation, "
            "CASE WHEN task.lease_until IS NOT NULL THEN 'running' WHEN task.is_done AND "
            "task.is_ok THEN 'completed' WHEN task.count_fails > 0 THEN 'retry' ELSE 'pending' END "
            "AS sync_status, task.count_fails AS sync_count_fails FROM sys_sync_task task WHERE "
            "task.resource_type = 'certificate' AND "
            "task.tenant_id = cert.tenant_id AND task.resource_id = cert.id AND task.version = "
            "cert.issuance_revision ORDER BY task.updated_at DESC LIMIT 1) latest_task ON TRUE "
            "WHERE cert.deleted_at IS NULL AND "
            "cert.tenant_id = $1";
        std::vector<ruvia::DbValue> params{ruvia::DbValue{tenantId}};
        std::optional<std::string> pattern;
        if (keyword) {
            pattern = "%" + service::common::escapeLikePattern(*keyword) + "%";
            where += " AND cert.domain ILIKE $" + std::to_string(params.size() + 1);
            params.emplace_back(std::string_view(*pattern));
        }
        if (status && !status->empty()) {
            where += " AND cert.status = $" + std::to_string(params.size() + 1);
            params.emplace_back(*status);
        }
        if (usable) {
            where += " AND COALESCE(cert.issued_revision > 0 AND cert.expires_at > NOW(), FALSE) "
                     "= $" +
                     std::to_string(params.size() + 1);
            params.emplace_back(*usable);
        }
        const auto countRows = co_await c.db().query("SELECT COUNT(*)" + where, params);
        const auto total = countRows.empty() ? std::int64_t{0}
                                             : countRows.front()[0].as<std::int64_t>().value_or(0);
        const auto rows =
            co_await c.db().query(certificateColumns() + where + " ORDER BY cert.sort DESC LIMIT " +
                                      std::to_string(pageSize) + " OFFSET " + std::to_string(skip),
                                  params);
        CertificatePageDataDto result(c);
        result.set<"total">(total);
        result.set<"page">(page);
        result.set<"pageSize">(pageSize);
        result.set<"totalPages">(pageSize > 0 ? (total + pageSize - 1) / pageSize : 0);
        auto& items = result.ensure<"list">();
        for (const auto& row : rows) {
            fillCertificate(c, items.emplace_back(c), row);
        }
        co_return result;
    }

    ruvia::Task<CertificateDto> get(ruvia::Context& c, const std::string& tenantId,
                                    const std::string& id) const {
        const auto rows = co_await c.db().query(
            certificateColumns() +
                " FROM sys_certificate cert INNER JOIN sys_dns_zone zone ON zone.tenant_id = "
                "cert.tenant_id AND zone.id = cert.dns_zone_id INNER JOIN sys_provider "
                "provider ON provider.tenant_id = cert.tenant_id AND provider.id = "
                "cert.provider_id AND provider.kind = 'certificate' LEFT JOIN LATERAL (SELECT "
                "task.operation, CASE WHEN task.lease_until IS NOT NULL THEN 'running' WHEN "
                "task.is_done AND task.is_ok THEN 'completed' WHEN task.count_fails > 0 THEN "
                "'retry' "
                "ELSE 'pending' END AS sync_status, task.count_fails AS sync_count_fails FROM "
                "sys_sync_task task "
                "WHERE "
                "task.resource_type = 'certificate' AND task.tenant_id = cert.tenant_id AND "
                "task.resource_id = cert.id AND task.version = cert.issuance_revision ORDER BY "
                "task.updated_at DESC LIMIT 1) latest_task ON TRUE WHERE cert.id = $1 AND "
                "cert.tenant_id = $2 AND "
                "cert.deleted_at IS NULL LIMIT 1",
            id, tenantId);
        if (rows.empty()) {
            service::common::throwAppError(CertificateError::NOT_FOUND);
        }
        CertificateDto result(c);
        fillCertificate(c, result, rows.front());
        co_return result;
    }

    ruvia::Task<service::certificate_material::CertificateDownload>
    download(ruvia::Context& c, const std::string& tenantId, const std::string& id) const {
        const auto rows = co_await c.db().query(
            "SELECT domain, material::text, COALESCE(issued_revision > 0 AND expires_at > NOW(), "
            "FALSE) FROM sys_certificate WHERE id = $1 AND tenant_id = $2 AND deleted_at IS "
            "NULL LIMIT 1",
            id, tenantId);
        if (rows.empty()) {
            service::common::throwAppError(CertificateError::NOT_FOUND);
        }
        if (!rows.front()[2].as<bool>().value_or(false)) {
            service::common::throwAppError(CertificateError::CERTIFICATE_UNAVAILABLE);
        }
        const auto material = service::certificate_material::parseStored(
            rows.front()[1].value().value_or("{}"), {.resource = c.resource()});
        if (!material) {
            throw std::runtime_error("stored certificate material is invalid");
        }
        if (!material->certificateChainPem || !material->privateKeyEnvelope) {
            service::common::throwAppError(CertificateError::CERTIFICATE_UNAVAILABLE);
        }
        const auto filename = service::certificate_material::archiveFilename(
            rows.front()[0].value().value_or("certificate"));
        auto archive = co_await c.runBlocking(
            [archiveFilename = filename, chain = std::move(*material->certificateChainPem),
             privateKey = service::utils::SensitiveString(
                 service::utils::openSecret(*material->privateKeyEnvelope))]() {
                return service::certificate_material::buildArchive(archiveFilename, chain,
                                                                   privateKey.view());
            });
        co_return service::certificate_material::CertificateDownload{
            filename + ".zip",
            std::move(archive),
        };
    }

  private:
    static std::string certificateColumns() {
        return "SELECT cert.id, cert.revision, cert.config::text, cert.status, "
               "cert.material::text, TO_CHAR(cert.expires_at, "
               "'YYYY-MM-DD\"T\"HH24:MI:SS.USOF'), cert.last_error, "
               "TO_CHAR(cert.created_at, 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF'), "
               "TO_CHAR(cert.updated_at, 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF'), "
               "cert.dns_zone_id, zone.domain, CASE WHEN cert.expires_at IS NULL THEN NULL ELSE "
               "GREATEST(0, FLOOR(EXTRACT(EPOCH FROM (cert.expires_at - NOW())) / 86400))::BIGINT "
               "END, cert.subject_alt_names[1], cert.subject_alt_names[2], provider.id, "
               "provider.provider, latest_task.sync_status, latest_task.sync_count_fails, "
               "(SELECT COUNT(DISTINCT website.id) FROM "
               "sys_website_certificate_binding binding INNER JOIN sys_website website ON "
               "website.id = binding.website_id AND website.tenant_id = cert.tenant_id "
               "AND website.deleted_at IS NULL WHERE binding.tenant_id = cert.tenant_id "
               "AND binding.certificate_id = cert.id), COALESCE(cert.issued_revision > 0 AND "
               "cert.expires_at > NOW(), FALSE)";
    }
};

inline const CertificateReadService& certificateReadService() {
    static const CertificateReadService service;
    return service;
}

} // namespace service::certificate
