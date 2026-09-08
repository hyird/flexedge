#pragma once

#include "service/features/sync_event/fanout.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

#include <ruvia/core/Task.h>
#include <ruvia/web/Context.h>
#include <ruvia/web/db/Db.h>
#include <ruvia/web/db/DbTransaction.h>

#include "service/common/database.h"
#include "service/common/http.h"
#include "service/domains/certificate/certificate.error.h"
#include "service/domains/certificate/certificate.types.h"
#include "service/features/certificate/model.h"
#include "service/features/certificate/dns_challenge.h"
#include "service/features/certificate/queue.h"
#include "service/features/dns/registry.h"
#include "service/features/sync_runtime/state.h"

namespace service::certificate {

class CertificateCommandService final {
  public:
    ruvia::Task<void> create(ruvia::Context& c, const std::string& tenantId,
                             const CreateCertificateBody& body) {
        const auto& domainInput = body.get<"domain">();
        const auto& providerIdInput = body.get<"certificateProviderId">();
        const auto& zoneIdInput = body.get<"dnsZoneId">();
        const auto& configInput = body.get<"config">();
        if (!domainInput || !providerIdInput || !zoneIdInput || !configInput) {
            service::common::throwAppError(service::common::kValidationErrorCode,
                                           "证书域名、供应商、托管域名和配置不能为空", 400);
        }
        const auto config = service::certificate_issuance::normalize(*configInput);
        if (!config) {
            service::common::throwAppError(service::common::kValidationErrorCode, "证书配置不正确",
                                           400);
        }
        const auto domain = normalizeDomain(domainInput->view());
        const auto providerId = std::string(providerIdInput->view());
        const auto zoneId = std::string(zoneIdInput->view());
        auto transaction = co_await c.db().beginTransaction();
        const auto exists = co_await transaction.query(
            "SELECT id FROM sys_certificate WHERE tenant_id = $1 AND domain = $2 AND "
            "deleted_at IS NULL LIMIT 1",
            tenantId, std::string_view(domain));
        if (!exists.empty()) {
            service::common::throwAppError(CertificateError::DOMAIN_EXISTS);
        }
        const auto providerRows = co_await transaction.query(
            "SELECT id FROM sys_provider WHERE id = $1 AND tenant_id = $2 AND kind = "
            "'certificate' AND status = 'verified' AND deleted_at IS NULL LIMIT 1 FOR SHARE",
            providerId, tenantId);
        if (providerRows.empty()) {
            service::common::throwAppError(CertificateError::PROVIDER_UNAVAILABLE);
        }
        const auto zoneRows = co_await transaction.query(
            "SELECT zone.domain, dns_provider.provider FROM sys_dns_zone zone INNER JOIN "
            "sys_provider dns_provider ON dns_provider.tenant_id = zone.tenant_id AND "
            "dns_provider.id = zone.provider_id AND dns_provider.kind = 'dns' WHERE zone.id = $1 "
            "AND zone.tenant_id = $2 AND zone.sync_status = 'synced' AND "
            "dns_provider.status = 'verified' AND zone.deleted_at IS NULL AND "
            "dns_provider.deleted_at IS NULL LIMIT 1 FOR SHARE OF zone, dns_provider",
            zoneId, tenantId);
        if (zoneRows.empty()) {
            service::common::throwAppError(CertificateError::DOMAIN_UNAVAILABLE);
        }
        if (!service::dns::findDnsProvider(zoneRows.front()[1].value().value_or(""))) {
            service::common::throwAppError(CertificateError::DNS_PROVIDER_UNSUPPORTED);
        }
        const auto zoneDomain = std::string(zoneRows.front()[0].value().value_or(""));
        const auto plainDomain = domain.starts_with("*.") ? domain.substr(2) : domain;
        if (plainDomain != zoneDomain &&
            (plainDomain.size() <= zoneDomain.size() || !plainDomain.ends_with("." + zoneDomain))) {
            service::common::throwAppError(CertificateError::DOMAIN_MISMATCH);
        }
        const auto configJson = serializeCertificateConfig(c, *config);
        std::string certificateId;
        try {
            const auto saved = co_await transaction.query(
                "INSERT INTO sys_certificate (tenant_id, provider_id, domain, dns_zone_id, "
                "status, config, material, revision, issuance_revision, issued_revision, "
                "created_at, updated_at) VALUES ($1, $2, $3, $4, 'pending', $5::jsonb, "
                "'{}'::jsonb, 1, 1, 0, NOW(), NOW()) RETURNING id",
                tenantId, providerId, std::string_view(domain), zoneId,
                std::string_view(configJson));
            certificateId = std::string(saved.front()[0].value().value_or(""));
        } catch (const ruvia::DbError& error) {
            if (service::common::isUniqueConstraintViolation(error, "uk_certificate_domain")) {
                service::common::throwAppError(CertificateError::DOMAIN_EXISTS);
            }
            throw;
        }
        (void)co_await service::certificate_issuance::enqueueCertificateRevision(
            transaction, tenantId, certificateId, 1, service::sync_runtime::MarkerOperation::issue);
        co_await transaction.commit();
        service::sync_event::fanout::hub().publish(tenantId);
        co_return;
    }

    ruvia::Task<void> update(
        ruvia::Context& c, const std::string& tenantId, const std::string& id,
        std::int64_t expectedRevision,
        const ruvia::ValidatedJson<service::certificate_issuance::CertificateConfigInput>& config) {
        auto transaction = co_await c.db().beginTransaction();
        const auto certificates = co_await transaction.query(
            "SELECT revision FROM sys_certificate WHERE id = $1 AND tenant_id = $2 AND "
            "deleted_at IS NULL LIMIT 1 FOR UPDATE",
            id, tenantId);
        if (certificates.empty()) {
            service::common::throwAppError(CertificateError::NOT_FOUND);
        }
        if (certificates.front()[0].as<std::int64_t>().value_or(0) != expectedRevision) {
            service::common::throwAppError(CertificateError::REVISION_CONFLICT);
        }
        const auto normalizedConfig = service::certificate_issuance::normalize(config.value());
        if (!normalizedConfig) {
            service::common::throwAppError(service::common::kValidationErrorCode, "证书配置不正确",
                                           400);
        }
        const auto configJson = serializeCertificateConfig(c, *normalizedConfig);
        const auto result = co_await transaction.execute(
            "UPDATE sys_certificate SET revision = revision + 1, config = $1::jsonb, updated_at = "
            "NOW() WHERE id = $2 AND tenant_id = $3 AND revision = $4 AND deleted_at IS NULL",
            std::string_view(configJson), id, tenantId, expectedRevision);
        if (result.affectedRows() == 0) {
            service::common::throwAppError(CertificateError::REVISION_CONFLICT);
        }
        co_await transaction.commit();
        service::sync_event::fanout::hub().publish(tenantId);
        co_return;
    }

    ruvia::Task<void> renew(ruvia::Context& c, const std::string& tenantId, const std::string& id,
                            std::int64_t expectedRevision) {
        auto transaction = co_await c.db().beginTransaction();
        const auto rows = co_await transaction.query(
            "SELECT revision, issuance_revision, issued_revision FROM sys_certificate WHERE id = "
            "$1 AND tenant_id = $2 AND deleted_at IS NULL LIMIT 1 FOR UPDATE",
            id, tenantId);
        if (rows.empty()) {
            service::common::throwAppError(CertificateError::NOT_FOUND);
        }
        if (rows.front()[0].as<std::int64_t>().value_or(0) != expectedRevision) {
            service::common::throwAppError(CertificateError::REVISION_CONFLICT);
        }
        const auto issuanceRevision = rows.front()[1].as<std::int64_t>().value_or(0) + 1;
        const auto hasCertificate = rows.front()[2].as<std::int64_t>().value_or(0) > 0;
        const auto result = co_await transaction.execute(
            "UPDATE sys_certificate SET issuance_revision = $1, status = $2, last_error = NULL, "
            "updated_at = NOW() WHERE id = $3 AND tenant_id = $4 AND revision = $5 AND "
            "deleted_at IS NULL",
            issuanceRevision,
            hasCertificate ? std::string_view{"renewing"} : std::string_view{"pending"}, id,
            tenantId, expectedRevision);
        if (result.affectedRows() == 0) {
            service::common::throwAppError(CertificateError::REVISION_CONFLICT);
        }
        (void)co_await service::certificate_issuance::enqueueCertificateRevision(
            transaction, tenantId, id, issuanceRevision,
            service::sync_runtime::MarkerOperation::renew);
        co_await transaction.commit();
        service::sync_event::fanout::hub().publish(tenantId);
        co_return;
    }

    ruvia::Task<void> remove(ruvia::Context& c, const std::string& tenantId, const std::string& id,
                             std::int64_t expectedRevision) {
        auto transaction = co_await c.db().beginTransaction();
        const auto certificates =
            co_await transaction.query("SELECT revision, dns_zone_id FROM sys_certificate WHERE id "
                                       "= $1 AND tenant_id = $2 AND "
                                       "deleted_at IS NULL LIMIT 1 FOR UPDATE",
                                       id, tenantId);
        if (certificates.empty()) {
            service::common::throwAppError(CertificateError::NOT_FOUND);
        }
        if (certificates.front()[0].as<std::int64_t>().value_or(0) != expectedRevision) {
            service::common::throwAppError(CertificateError::REVISION_CONFLICT);
        }
        const auto dnsZoneId = certificates.front()[1].value().value_or("");
        const auto consumers = co_await transaction.query(
            "SELECT website_id FROM sys_website_certificate_binding WHERE tenant_id = $1 AND "
            "certificate_id = $2 LIMIT 1",
            tenantId, id);
        if (!consumers.empty()) {
            service::common::throwAppError(CertificateError::IN_USE);
        }
        const auto result = co_await transaction.execute(
            "UPDATE sys_certificate SET material = '{}'::jsonb, revision = revision + 1, "
            "deleted_at = NOW(), updated_at = NOW() WHERE id = $1 AND tenant_id = $2 AND "
            "revision = $3 AND deleted_at IS NULL",
            id, tenantId, expectedRevision);
        if (result.affectedRows() == 0) {
            service::common::throwAppError(CertificateError::NOT_FOUND);
        }
        co_await service::certificate_issuance::retireDnsChallengesForDeletedCertificate(
            transaction, tenantId, dnsZoneId, id);
        co_await service::sync_runtime::removeMarker(
            transaction, tenantId, service::sync_runtime::MarkerResourceType::certificate, id);
        co_await transaction.commit();
        service::sync_event::fanout::hub().publish(tenantId);
        co_return;
    }

  private:
    static std::string
    serializeCertificateConfig(ruvia::Context& c,
                               const service::certificate_issuance::CertificateConfigData& config) {
        const auto output =
            service::certificate_issuance::toOutput(config, {.resource = c.resource()});
        const auto json = ruvia::toJson(output, {.resource = c.resource()});
        return std::string(json.data(), json.size());
    }

    static std::string normalizeDomain(std::string_view input) {
        std::string result(input);
        std::transform(result.begin(), result.end(), result.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return result;
    }
};

inline CertificateCommandService& certificateCommandService() {
    static CertificateCommandService service;
    return service;
}

} // namespace service::certificate
