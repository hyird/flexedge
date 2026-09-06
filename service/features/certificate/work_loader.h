#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <ruvia/core/Task.h>

#include "service/features/background/worker_pool.h"
#include "service/features/certificate/acme_types.h"
#include "service/features/certificate/provider_config.h"
#include "service/features/certificate/task.h"
#include "service/features/dns/driver.h"
#include "service/utils/secret.h"
#include "service/utils/sensitive_string.h"

namespace service::certificate_issuance::worker_detail {

struct CertificateWork final {
    std::string tenantId;
    std::string providerId;
    std::string provider;
    std::int64_t providerRevision;
    std::optional<std::string> accountEmail;
    std::optional<EabCredentials> eab;
    std::vector<std::string> domains;
    std::string dnsZoneId;
    std::string zoneDomain;
    std::int64_t dnsMinimumRecordTtl;
};

inline ruvia::Task<CertificateWork> loadWork(service::background::WorkerContext& context,
                                             const CertificateTask& task) {
    const auto rows = co_await context.db().query(
        "SELECT certificate.tenant_id, certificate.domain, certificate.subject_alt_names[1], "
        "certificate.subject_alt_names[2], certificate.provider_id, certificate.dns_zone_id, "
        "provider.provider, provider.status, provider.revision, provider.config::text, "
        "provider.runtime::text, "
        "zone.domain, dns_provider.provider, dns_provider.status FROM sys_certificate certificate "
        "INNER JOIN "
        "sys_provider provider ON provider.tenant_id = certificate.tenant_id AND "
        "provider.id = certificate.provider_id AND provider.kind = 'certificate' INNER JOIN "
        "sys_dns_zone zone ON zone.tenant_id = certificate.tenant_id AND zone.id = "
        "certificate.dns_zone_id INNER JOIN sys_provider dns_provider ON "
        "dns_provider.tenant_id = zone.tenant_id AND dns_provider.id = zone.provider_id AND "
        "dns_provider.kind = 'dns' WHERE certificate.id = $1 AND certificate.issuance_revision = "
        "$2 AND certificate.tenant_id = $3 AND certificate.deleted_at IS NULL AND "
        "provider.deleted_at IS NULL AND zone.deleted_at IS "
        "NULL AND dns_provider.deleted_at IS NULL LIMIT 1",
        task.certificateId, task.version, task.tenantId);
    if (rows.empty()) {
        throw AcmeError("证书任务对应的资源不存在或版本已变化", true);
    }
    const auto& row = rows.front();
    if (row[7].value().value_or("") != "verified") {
        throw AcmeError("证书供应商尚未通过检测", false);
    }
    if (row[13].value().value_or("") != "verified") {
        throw AcmeError("DNS 服务商尚未通过检测", false);
    }

    CertificateProviderConfigData providerConfig;
    try {
        providerConfig =
            parseCertificateProviderConfig(row[9].value().value_or("{}"), context.resource());
    } catch (const std::runtime_error&) {
        throw AcmeError("证书供应商配置损坏", true);
    }
    std::optional<std::string> accountEmail = providerConfig.accountEmail;

    std::optional<EabCredentials> eab;
    const auto provider = std::string(row[6].value().value_or(""));
    if (provider == "zerossl") {
        const auto runtime = parseCertificateProviderRuntime(row[10].value().value_or("{}"),
                                                             {.resource = context.resource()});
        if (!runtime) {
            throw AcmeError("ZeroSSL EAB 凭据不存在，请重新检测证书供应商", true);
        }
        const auto& eabInput = runtime->eab;
        if (!eabInput) {
            throw AcmeError("ZeroSSL EAB 凭据不存在，请重新检测证书供应商", true);
        }
        const auto& keyId = eabInput->kid;
        const auto& hmacKeyEnvelope = eabInput->hmacKeyEnvelope;
        if (!keyId || !hmacKeyEnvelope) {
            throw AcmeError("ZeroSSL EAB 凭据不存在，请重新检测证书供应商", true);
        }
        eab = EabCredentials{
            *keyId,
            service::utils::SensitiveString(service::utils::openSecret(*hmacKeyEnvelope)),
        };
    }

    std::vector<std::string> domains;
    domains.reserve(3);
    for (const auto index : {1U, 2U, 3U}) {
        const auto& value = row[index];
        if (const auto domain = value.value(); domain && !domain->empty()) {
            domains.emplace_back(*domain);
        }
    }
    if (domains.empty()) {
        throw AcmeError("证书域名为空", true);
    }
    co_return CertificateWork{
        .tenantId = std::string(row[0].value().value_or("")),
        .providerId = std::string(row[4].value().value_or("")),
        .provider = provider,
        .providerRevision = row[8].as<std::int64_t>().value_or(0),
        .accountEmail = std::move(accountEmail),
        .eab = std::move(eab),
        .domains = std::move(domains),
        .dnsZoneId = std::string(row[5].value().value_or("")),
        .zoneDomain = std::string(row[11].value().value_or("")),
        .dnsMinimumRecordTtl =
            service::dns::DnsProviderDriver(row[12].value().value_or("")).minimumRecordTtl(),
    };
}

} // namespace service::certificate_issuance::worker_detail
