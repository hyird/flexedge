#pragma once

#include <ruvia/core/task.h>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "service/features/background/task_context.h"
#include "service/features/certificate/acme_types.h"
#include "service/features/certificate/issuance_state.h"
#include "service/features/certificate/provider_config_mapper.h"
#include "service/features/certificate/task.h"
#include "service/features/dns/driver.h"
#include "service/utils/secret.h"
#include "service/utils/sensitive_string.h"

namespace service::certificate_issuance::worker_detail {

struct certificate_work final {
    std::string tenant_id_;
    std::string provider_id_;
    std::string provider_;
    std::int64_t provider_revision_;
    std::optional<std::string> account_email_;
    std::optional<eab_credentials> eab_;
    std::vector<std::string> domains_;
    std::string dns_zone_id_;
    std::string zone_domain_;
    std::int64_t dns_minimum_record_ttl_;
    // Progress a previous attempt of this issuance revision left to resume.
    std::optional<issuance_state> progress_;
};

inline ruvia::task<certificate_work> load_work(service::background::task_context& context,
    const certificate_task& task) {
    const auto rows = co_await context.db().query(
        "SELECT certificate.tenant_id, certificate.subject_alt_names[1], "
        "certificate.subject_alt_names[2], certificate.provider_id, certificate.dns_zone_id, "
        "provider.provider, provider.status, provider.revision, provider.config::text, "
        "provider.runtime::text, "
        "zone.domain, dns_provider.provider, dns_provider.status, certificate.issuance_state::text FROM "
        "sys_certificate certificate "
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
        task.certificate_id_, task.version_, task.tenant_id_);
    if (rows.empty()) {
        throw acme_error("证书任务对应的资源不存在或版本已变化", true);
    }
    const auto& row = rows.front();
    if (row[6].value().value_or("") != "verified") {
        throw acme_error("证书供应商尚未通过检测", false);
    }
    if (row[12].value().value_or("") != "verified") {
        throw acme_error("DNS 服务商尚未通过检测", false);
    }

    certificate_provider_config_data provider_config;
    try {
        provider_config =
            parse_certificate_provider_config(row[8].value().value_or("{}"), context.pool());
    } catch (const std::runtime_error&) {
        throw acme_error("证书供应商配置损坏", true);
    }
    std::optional<std::string> account_email = provider_config.account_email_;

    std::optional<eab_credentials> eab;
    const auto provider = std::string(row[5].value().value_or(""));
    if (provider == "zerossl") {
        const auto runtime = parse_certificate_provider_runtime(row[9].value().value_or("{}"),
            {.resource_ = context.pool()});
        if (!runtime) {
            throw acme_error("ZeroSSL EAB 凭据不存在，请重新检测证书供应商", true);
        }
        const auto& eab_input = runtime->eab_;
        if (!eab_input) {
            throw acme_error("ZeroSSL EAB 凭据不存在，请重新检测证书供应商", true);
        }
        const auto& key_id = eab_input->kid_;
        const auto& hmac_key_envelope = eab_input->hmac_key_envelope_;
        if (!key_id || !hmac_key_envelope) {
            throw acme_error("ZeroSSL EAB 凭据不存在，请重新检测证书供应商", true);
        }
        eab = eab_credentials{
            *key_id,
            service::utils::sensitive_string(service::utils::open_secret(*hmac_key_envelope)),
        };
    }

    std::vector<std::string> domains;
    domains.reserve(2);
    // The generated SAN array already includes the certificate's primary domain.
    for (const auto index : {1U, 2U}) {
        const auto& value = row[index];
        if (const auto domain = value.value(); domain && !domain->empty()) {
            domains.emplace_back(*domain);
        }
    }
    if (domains.empty()) {
        throw acme_error("证书域名为空", true);
    }
    co_return certificate_work{
        .tenant_id_ = std::string(row[0].value().value_or("")),
        .provider_id_ = std::string(row[3].value().value_or("")),
        .provider_ = provider,
        .provider_revision_ = row[7].as<std::int64_t>().value_or(0),
        .account_email_ = std::move(account_email),
        .eab_ = std::move(eab),
        .domains_ = std::move(domains),
        .dns_zone_id_ = std::string(row[4].value().value_or("")),
        .zone_domain_ = std::string(row[10].value().value_or("")),
        .dns_minimum_record_ttl_ =
            service::dns::dns_provider_driver(row[11].value().value_or("")).minimum_record_ttl(),
        .progress_ = parse_issuance_state(row[13].value().value_or(""), task.version_, context.pool()),
    };
}

}  // namespace service::certificate_issuance::worker_detail
