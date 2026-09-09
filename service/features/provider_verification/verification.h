#pragma once

#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ruvia/core/Task.h>

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

struct VerificationResult final {
    std::optional<std::string> dnsRuntime{};
    std::optional<service::certificate_issuance::EabCredentials> eab{};
};

inline std::optional<service::certificate_issuance::EabCredentials>
parseRuntimeEab(std::string_view currentRuntimeJson, std::pmr::memory_resource* resource) {
    const auto runtime = service::certificate_issuance::parseCertificateProviderRuntime(
        currentRuntimeJson, {.resource = resource});
    if (!runtime) {
        throw VerificationError("证书供应商 runtime 损坏", true);
    }
    const auto& runtimeEab = runtime->eab;
    if (!runtimeEab) {
        return std::nullopt;
    }
    const auto& kid = runtimeEab->kid;
    const auto& hmacKeyEnvelope = runtimeEab->hmacKeyEnvelope;
    if (!kid || !hmacKeyEnvelope) {
        return std::nullopt;
    }
    try {
        return service::certificate_issuance::EabCredentials{
            *kid,
            service::utils::SensitiveString(service::utils::openSecret(*hmacKeyEnvelope)),
        };
    } catch (...) {
        throw VerificationError("证书供应商 EAB runtime 损坏", true);
    }
}

template <typename Runtime>
ruvia::Task<service::certificate_issuance::EabCredentials>
fetchZeroSslEab(Runtime& context,
                const service::certificate_issuance::CertificateProviderConfigData& config,
                std::string_view credentialMode) {
    if (credentialMode == "email") {
        if (!config.accountEmail) {
            throw VerificationError("ZeroSSL 供应商配置不完整", true);
        }
        co_return co_await service::certificate_issuance::fetchZeroSslEabByEmail(
            context, *config.accountEmail);
    }
    if (credentialMode == "access_key") {
        if (!config.envelope) {
            throw VerificationError("ZeroSSL 供应商配置不完整", true);
        }
        service::utils::SensitiveString accessKey([&] {
            try {
                return service::utils::openSecret(*config.envelope);
            } catch (...) {
                throw VerificationError("证书供应商 Access Key 损坏", true);
            }
        }());
        co_return co_await service::certificate_issuance::fetchZeroSslEabByAccessKey(
            context, accessKey.view());
    }
    throw VerificationError("ZeroSSL 供应商配置不完整", true);
}

inline ruvia::Task<VerificationResult> verifyDns(service::background::WorkerContext& context,
                                                 const VerificationTask& task) {
    const auto config = service::dns::parseDnsProviderConfig(task.configJson, context.resource());
    service::utils::SensitiveString secret([&] {
        try {
            return service::utils::openSecret(config.credentialEnvelope);
        } catch (...) {
            throw VerificationError("DNS 服务商凭据损坏", true);
        }
    }());
    const service::dns::DnsProviderDriver driver(task.provider);
    const auto zones = co_await driver.verify(context, task.accountId, secret.view());
    std::vector<service::dns::DnsProviderZoneRuntimeData> runtimeZones;
    runtimeZones.reserve(zones.size());
    for (const auto& zone : zones) {
        runtimeZones.push_back({.domain = zone.name, .status = zone.status});
    }
    co_return VerificationResult{
        .dnsRuntime = service::dns::serializeDnsProviderRuntime(runtimeZones, context.resource()),
    };
}

inline ruvia::Task<VerificationResult>
verifyCertificate(service::background::WorkerContext& context, const VerificationTask& task,
                  std::string_view currentRuntimeJson) {
    const auto config = service::certificate_issuance::parseCertificateProviderConfig(
        task.configJson, context.resource());
    const auto settings = service::certificate_issuance::settingsForProvider(task.provider);
    if (!settings) {
        throw VerificationError("不支持的证书供应商", true);
    }
    std::optional<service::certificate_issuance::EabCredentials> eab;
    if (task.provider == "zerossl") {
        eab = parseRuntimeEab(currentRuntimeJson, context.resource());
        if (!eab) {
            eab = co_await fetchZeroSslEab(context, config, config.credentialMode);
        }
    }
    co_await service::certificate_issuance::verifyAcmeDirectory(context, *settings);
    co_return VerificationResult{.eab = std::move(eab)};
}

} // namespace service::provider_verification::detail
