#pragma once

#include <ruvia/core/task.h>

#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "service/features/background/worker_pool.h"
#include "service/features/certificate/provider.h"
#include "service/features/certificate/provider_config_mapper.h"
#include "service/features/dns/driver.h"
#include "service/features/dns/provider_config_mapper.h"
#include "service/features/dns/provider_runtime.h"
#include "service/features/provider_verification/failure.h"
#include "service/features/provider_verification/task.h"
#include "service/utils/secret.h"
#include "service/utils/sensitive_string.h"

namespace service::provider_verification::detail {

struct verification_result final {
    std::optional<std::string> dns_runtime_{};
    std::optional<service::certificate_issuance::eab_credentials> eab_{};
};

inline std::optional<service::certificate_issuance::eab_credentials>
parse_runtime_eab(std::string_view current_runtime_json, std::pmr::memory_resource* resource) {
    const auto runtime = service::certificate_issuance::parse_certificate_provider_runtime(
        current_runtime_json, {.resource_ = resource});
    if (!runtime) {
        throw verification_error("证书供应商 runtime 损坏", true);
    }
    const auto& runtime_eab = runtime->eab_;
    if (!runtime_eab) {
        return std::nullopt;
    }
    const auto& kid = runtime_eab->kid_;
    const auto& hmac_key_envelope = runtime_eab->hmac_key_envelope_;
    if (!kid || !hmac_key_envelope) {
        return std::nullopt;
    }
    try {
        return service::certificate_issuance::eab_credentials{
            *kid,
            service::utils::sensitive_string(service::utils::open_secret(*hmac_key_envelope)),
        };
    } catch (...) {
        throw verification_error("证书供应商 EAB runtime 损坏", true);
    }
}

template <typename runtime_type>
ruvia::task<service::certificate_issuance::eab_credentials>
fetch_zero_ssl_eab(runtime_type& context,
    const service::certificate_issuance::certificate_provider_config_data& config,
    std::string_view credential_mode) {
    if (credential_mode == "email") {
        if (!config.account_email_) {
            throw verification_error("ZeroSSL 供应商配置不完整", true);
        }
        co_return co_await service::certificate_issuance::fetch_zero_ssl_eab_by_email(
            context, *config.account_email_);
    }
    if (credential_mode == "access_key") {
        if (!config.envelope_) {
            throw verification_error("ZeroSSL 供应商配置不完整", true);
        }
        service::utils::sensitive_string access_key([&] {
            try {
                return service::utils::open_secret(*config.envelope_);
            } catch (...) {
                throw verification_error("证书供应商 Access Key 损坏", true);
            }
        }());
        co_return co_await service::certificate_issuance::fetch_zero_ssl_eab_by_access_key(
            context, access_key.view());
    }
    throw verification_error("ZeroSSL 供应商配置不完整", true);
}

inline ruvia::task<verification_result> verify_dns(service::background::worker_context& context,
    const verification_task& task) {
    const auto config = service::dns::parse_dns_provider_config(task.config_json_, context.pool());
    service::utils::sensitive_string secret([&] {
        try {
            return service::utils::open_secret(config.credential_envelope_);
        } catch (...) {
            throw verification_error("DNS 服务商凭据损坏", true);
        }
    }());
    const service::dns::dns_provider_driver driver(task.provider_);
    const auto zones = co_await driver.verify(context, task.account_id_, secret.view());
    std::vector<service::dns::dns_provider_zone_runtime_data> runtime_zones;
    runtime_zones.reserve(zones.size());
    for (const auto& zone : zones) {
        runtime_zones.push_back({.domain_ = zone.name_, .status_ = zone.status_});
    }
    co_return verification_result{
        .dns_runtime_ = service::dns::serialize_dns_provider_runtime(runtime_zones, context.pool()),
    };
}

inline ruvia::task<verification_result>
verify_certificate(service::background::worker_context& context, const verification_task& task,
    std::string_view current_runtime_json) {
    const auto config = service::certificate_issuance::parse_certificate_provider_config(
        task.config_json_, context.pool());
    const auto settings = service::certificate_issuance::settings_for_provider(task.provider_);
    if (!settings) {
        throw verification_error("不支持的证书供应商", true);
    }
    std::optional<service::certificate_issuance::eab_credentials> eab;
    if (task.provider_ == "zerossl") {
        eab = parse_runtime_eab(current_runtime_json, context.pool());
        if (!eab) {
            eab = co_await fetch_zero_ssl_eab(context, config, config.credential_mode_);
        }
    }
    co_await service::certificate_issuance::verify_acme_directory(context, *settings);
    co_return verification_result{.eab_ = std::move(eab)};
}

}  // namespace service::provider_verification::detail
