#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/context.h>
#include <ruvia/web/db/db.h>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "service/common/http.h"
#include "service/domains/certificate/certificate_error.h"
#include "service/domains/certificate/certificate_filter.h"
#include "service/domains/certificate/certificate_mapper.h"
#include "service/domains/certificate/certificate_types.h"
#include "service/features/certificate_material/archive.h"
#include "service/features/certificate_material/download.h"
#include "service/features/certificate_material/model.h"
#include "service/utils/secret.h"
#include "service/utils/sensitive_string.h"

namespace service::certificate {

class certificate_read_service final {
public:
    ruvia::task<certificate_page_data_dto>
    list(auto& c, const std::string& tenant_id, std::int64_t page, std::int64_t page_size,
        std::int64_t skip, const std::optional<std::string>& keyword,
        std::optional<std::string_view> status, std::optional<bool> usable, bool expiring,
        bool abnormal) const {
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
        std::vector<ruvia::db_value> params{ruvia::db_value{tenant_id}};
        std::optional<std::string> pattern;
        if (keyword) {
            pattern = "%" + service::common::escape_like_pattern(*keyword) + "%";
            where += " AND cert.domain ILIKE $" + std::to_string(params.size() + 1);
            params.emplace_back(std::string_view(*pattern));
        }
        if (status && !status->empty()) {
            where += " AND cert.status = $" + std::to_string(params.size() + 1);
            params.emplace_back(*status);
        }
        if (usable) {
            where +=
                " AND COALESCE(cert.issued_revision > 0 AND cert.expires_at > NOW(), FALSE) "
                "= $" +
                std::to_string(params.size() + 1);
            params.emplace_back(*usable);
        }
        if (expiring) {
            where += " AND " + certificate_expiring_sql();
        }
        if (abnormal) {
            where += " AND " + certificate_abnormal_sql();
        }
        const auto count_rows = co_await c.db().query("SELECT COUNT(*)" + where, params);
        const auto total = count_rows.empty() ? std::int64_t{0}
                                              : count_rows.front()[0].template as<std::int64_t>().value_or(0);
        const auto rows =
            co_await c.db().query(certificate_columns() + where + " ORDER BY cert.sort DESC LIMIT " +
                                      std::to_string(page_size) + " OFFSET " + std::to_string(skip),
                params);
        certificate_page_data_dto result({.resource_ = c.pool()});
        result.template set<"total">(total);
        result.template set<"page">(page);
        result.template set<"page_size">(page_size);
        result.template set<"total_pages">(page_size > 0 ? (total + page_size - 1) / page_size : 0);
        auto& items = result.template ensure<"list">();
        for (const auto& row : rows) {
            fill_certificate(c, items.emplace_back(), row);
        }
        co_return result;
    }

    ruvia::task<certificate_dto> get(auto& c, const std::string& tenant_id,
        const std::string& id) const {
        const auto rows = co_await c.db().query(
            certificate_columns() +
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
            id, tenant_id);
        if (rows.empty()) {
            service::common::throw_app_error(certificate_error::not_found);
        }
        certificate_dto result({.resource_ = c.pool()});
        fill_certificate(c, result, rows.front());
        co_return result;
    }

    ruvia::task<service::certificate_material::certificate_download>
    download(auto& c, const std::string& tenant_id, const std::string& id) const {
        const auto rows = co_await c.db().query(
            "SELECT domain, material::text, COALESCE(issued_revision > 0 AND expires_at > NOW(), "
            "FALSE) FROM sys_certificate WHERE id = $1 AND tenant_id = $2 AND deleted_at IS "
            "NULL LIMIT 1",
            id, tenant_id);
        if (rows.empty()) {
            service::common::throw_app_error(certificate_error::not_found);
        }
        if (!rows.front()[2].template as<bool>().value_or(false)) {
            service::common::throw_app_error(certificate_error::certificate_unavailable);
        }
        const auto material = service::certificate_material::parse_stored(
            rows.front()[1].value().value_or("{}"), {.resource_ = c.pool()});
        if (!material) {
            throw std::runtime_error("stored certificate material is invalid");
        }
        if (!material->certificate_chain_pem_ || !material->private_key_envelope_) {
            service::common::throw_app_error(certificate_error::certificate_unavailable);
        }
        const auto filename = service::certificate_material::archive_filename(
            rows.front()[0].value().value_or("certificate"));
        auto archive = co_await c.run_blocking(
            [archive_filename = filename, chain = std::move(*material->certificate_chain_pem_),
                private_key = service::utils::sensitive_string(
                    service::utils::open_secret(*material->private_key_envelope_))]() {
                return service::certificate_material::build_archive(archive_filename, chain,
                    private_key.view());
            });
        co_return service::certificate_material::certificate_download{
            filename + ".zip",
            std::move(archive),
        };
    }

private:
    static std::string certificate_columns() {
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

inline const certificate_read_service& get_certificate_read_service() {
    static const certificate_read_service service;
    return service;
}

}  // namespace service::certificate
