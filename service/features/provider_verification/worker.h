#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ruvia/core/Task.h>
#include <ruvia/core/Timer.h>

#include "service/features/background/worker_pool.h"
#include "service/features/certificate/provider.h"
#include "service/features/certificate/provider_config.h"
#include "service/features/dns/driver.h"
#include "service/features/dns/provider_config.h"
#include "service/features/dns/provider_runtime.h"
#include "service/features/dns_sync/queue.h"
#include "service/features/logging/logger.h"
#include "service/features/provider_verification/model.h"
#include "service/features/provider_verification/queue.h"
#include "service/features/sync_runtime/state.h"
#include "service/utils/secret.h"
#include "service/utils/sensitive_string.h"

namespace service::provider_verification {

namespace detail {

inline constexpr std::chrono::seconds kIdlePollInterval{2};
inline constexpr std::chrono::seconds kLeaseRecoveryInterval{15};
inline constexpr std::chrono::minutes kReconciliationInterval{15};
inline constexpr std::size_t kMaxJobsPerTick{8};

class VerificationError final : public std::runtime_error {
  public:
    VerificationError(std::string_view message, bool permanent)
        : std::runtime_error(std::string(message)), permanent_(permanent) {}

    [[nodiscard]] bool permanent() const noexcept { return permanent_; }

  private:
    bool permanent_;
};

struct VerificationTask final {
    std::string id;
    std::string tenantId;
    std::string providerId;
    std::string kind;
    std::string provider;
    std::string name;
    std::string accountId;
    std::int64_t providerRevision;
    std::int64_t generation;
    std::int64_t failures;
    std::string configJson;
    std::string runtimeJson;
};

struct VerificationResult final {
    std::optional<std::string> dnsRuntime{};
    std::optional<service::certificate_issuance::EabCredentials> eab{};
};

inline std::string boundedError(std::string_view value) {
    constexpr std::size_t limit{1000};
    return std::string(value.substr(0, std::min(value.size(), limit)));
}

inline ruvia::Task<void> reconcile(service::background::WorkerContext& context) {
    const auto rows = co_await context.db().query(
        "SELECT marker.tenant_id, marker.id, marker.resource_id, provider.verification_generation "
        "FROM sys_sync_task marker INNER JOIN sys_provider provider ON provider.tenant_id = "
        "marker.tenant_id AND provider.id = marker.resource_id WHERE marker.resource_type = "
        "'provider' AND provider.deleted_at IS NULL AND marker.version <> "
        "provider.verification_generation LIMIT 64");
    for (const auto& row : rows) {
        auto transaction = co_await context.db().beginTransaction();
        const auto providerId = std::string(row[2].value().value_or(""));
        co_await service::provider_verification::markCurrent(
            transaction, row[0].value().value_or(""), providerId);
        co_await transaction.commit();
    }
    co_return;
}

inline ruvia::Task<void> recoverStaleMarkers(service::background::WorkerContext& context) {
    co_await service::sync_runtime::recoverStaleRunning(context.db(), "provider");
    co_return;
}

inline ruvia::Task<std::optional<VerificationTask>>
claim(service::background::WorkerContext& context) {
    const auto rows = co_await context.db().query(
        "WITH candidate AS (SELECT marker.id, provider.kind, provider.provider, provider.name, "
        "provider.account_id, provider.revision, provider.config::text AS config, "
        "provider.runtime::text AS runtime FROM sys_sync_task marker INNER JOIN sys_provider "
        "provider ON provider.tenant_id = marker.tenant_id AND provider.id = marker.resource_id "
        "WHERE marker.resource_type = 'provider' AND marker.operation = 'verify' AND NOT "
        "marker.is_done AND marker.next_attempt_at <= NOW() AND marker.lease_until IS NULL AND "
        "provider.deleted_at IS NULL AND provider.verification_generation = marker.version "
        "ORDER BY marker.next_attempt_at ASC, marker.updated_at ASC FOR UPDATE OF marker SKIP "
        "LOCKED LIMIT 1) UPDATE sys_sync_task marker SET lease_owner = $1, lease_until = NOW() + "
        "INTERVAL '60 seconds', updated_at = NOW() FROM candidate WHERE marker.id = candidate.id "
        "RETURNING marker.id, marker.tenant_id, marker.resource_id, marker.version, "
        "marker.count_fails, candidate.kind, candidate.provider, candidate.name, "
        "candidate.account_id, "
        "candidate.revision, candidate.config, candidate.runtime",
        context.leaseOwner());
    if (rows.empty()) {
        co_return std::nullopt;
    }
    const auto& row = rows.front();
    co_return VerificationTask{
        .id = std::string(row[0].value().value_or("")),
        .tenantId = std::string(row[1].value().value_or("")),
        .providerId = std::string(row[2].value().value_or("")),
        .kind = std::string(row[5].value().value_or("")),
        .provider = std::string(row[6].value().value_or("")),
        .name = std::string(row[7].value().value_or("")),
        .accountId = std::string(row[8].value().value_or("")),
        .providerRevision = row[9].as<std::int64_t>().value_or(0),
        .generation = row[3].as<std::int64_t>().value_or(0),
        .failures = row[4].as<std::int64_t>().value_or(0),
        .configJson = std::string(row[10].value().value_or("{}")),
        .runtimeJson = std::string(row[11].value().value_or("{}")),
    };
}

inline ruvia::Task<std::optional<std::string>>
loadCurrentRuntime(service::background::WorkerContext& context, const VerificationTask& task) {
    const service::sync_runtime::RunningMarkerLease lease{
        .marker = {.tenantId = task.tenantId, .markerId = task.id, .version = task.generation},
        .owner = context.leaseOwner(),
    };
    if (!co_await service::sync_runtime::renewRunningLease(context.db(), lease)) {
        throw std::runtime_error("供应商检测标记 lease 已失效");
    }
    const auto rows = co_await context.db().query(
        "SELECT runtime::text FROM sys_provider WHERE id = $1 AND tenant_id = $2 AND kind = $3 "
        "AND revision = $4 AND verification_generation = $5 AND deleted_at IS NULL LIMIT 1",
        task.providerId, task.tenantId, task.kind, task.providerRevision, task.generation);
    if (!rows.empty()) {
        co_return std::string(rows.front()[0].value().value_or("{}"));
    }
    auto transaction = co_await context.db().beginTransaction();
    (void)co_await service::sync_runtime::releaseRunning(transaction, lease);
    co_await transaction.commit();
    co_return std::nullopt;
}

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

inline ruvia::Task<void> completeMarker(service::background::WorkerContext& context,
                                        const VerificationTask& task, VerificationResult result) {
    const bool dns = task.kind == "dns";
    const service::sync_runtime::RunningMarkerLease lease{
        .marker = {.tenantId = task.tenantId, .markerId = task.id, .version = task.generation},
        .owner = context.leaseOwner(),
    };
    auto transaction = co_await context.db().beginTransaction();
    const auto rows = co_await transaction.query(
        "SELECT runtime::text FROM sys_provider WHERE id = $1 AND tenant_id = $2 AND kind = $3 "
        "AND revision = $4 AND verification_generation = $5 AND deleted_at IS NULL LIMIT 1 FOR "
        "UPDATE",
        task.providerId, task.tenantId, task.kind, task.providerRevision, task.generation);
    if (rows.empty()) {
        (void)co_await service::sync_runtime::releaseRunning(transaction, lease);
        co_await transaction.commit();
        co_return;
    }
    if (!co_await service::sync_runtime::renewRunningLease(transaction, lease)) {
        throw std::runtime_error("供应商检测标记 lease 已失效");
    }

    std::string runtimeJson;
    if (dns) {
        if (!result.dnsRuntime) {
            throw VerificationError("DNS 服务商检测结果缺失", true);
        }
        runtimeJson = std::move(*result.dnsRuntime);
    } else {
        const auto runtime = service::certificate_issuance::parseCertificateProviderRuntime(
            rows.front()[0].value().value_or("{}"), {.resource = context.resource()});
        if (!runtime) {
            throw VerificationError("证书供应商 runtime 损坏", true);
        }
        runtimeJson = service::certificate_issuance::serializeCertificateProviderRuntime(
            runtime, result.eab, context.resource());
    }
    const auto updated = co_await transaction.execute(
        "UPDATE sys_provider SET runtime = $1::jsonb, status = 'verified', last_verified_at = "
        "NOW(), last_error = NULL, updated_at = NOW() WHERE id = $2 AND tenant_id = $3 AND "
        "revision = $4 AND verification_generation = $5 AND deleted_at IS NULL",
        std::string_view(runtimeJson), task.providerId, task.tenantId, task.providerRevision,
        task.generation);
    if (updated.affectedRows() == 0) {
        (void)co_await service::sync_runtime::releaseRunning(transaction, lease);
    } else {
        if (dns) {
            co_await service::dns_sync::markProviderZonesDirty(transaction, task.tenantId,
                                                               task.providerId);
        }
        if (!co_await service::sync_runtime::completeRunning(transaction, lease)) {
            throw std::runtime_error("供应商检测标记 lease 已失效");
        }
    }
    co_await transaction.commit();
    co_return;
}

inline ruvia::Task<void> execute(service::background::WorkerContext& context,
                                 const VerificationTask& task) {
    const auto currentRuntime = co_await loadCurrentRuntime(context, task);
    if (!currentRuntime) {
        co_return;
    }
    VerificationResult result;
    if (task.kind == "dns") {
        result = co_await verifyDns(context, task);
    } else if (task.kind == "certificate") {
        result = co_await verifyCertificate(context, task, *currentRuntime);
    } else {
        throw VerificationError("供应商类型无效", true);
    }
    co_await completeMarker(context, task, std::move(result));
}

inline ruvia::Task<void> fail(service::background::WorkerContext& context,
                              const VerificationTask& task, std::string_view error,
                              bool permanent) {
    const auto message = boundedError(error);
    const service::sync_runtime::RunningMarkerLease lease{
        .marker = {.tenantId = task.tenantId, .markerId = task.id, .version = task.generation},
        .owner = context.leaseOwner(),
    };
    auto transaction = co_await context.db().beginTransaction();
    const bool transitioned =
        co_await service::sync_runtime::failRunning(transaction, lease, message);
    if (transitioned) {
        (void)co_await transaction.execute(
            "UPDATE sys_provider SET status = CASE WHEN $1::BOOLEAN THEN 'invalid' ELSE status "
            "END, last_error = $2, updated_at = NOW() WHERE id = $3 AND tenant_id = $4 AND "
            "verification_generation = $5 AND deleted_at IS NULL",
            permanent, std::string_view(message), task.providerId, task.tenantId, task.generation);
    }
    co_await transaction.commit();
    if (transitioned) {
        service::logging::error("Provider verification marker " + task.id + " failed: " + message);
    }
    co_return;
}

struct TaskFailure final {
    std::string message;
    bool permanent{};
};

inline TaskFailure classifyTaskFailure(const std::exception_ptr& exception) {
    try {
        std::rethrow_exception(exception);
    } catch (const std::exception& error) {
        if (const auto* cloudflare = dynamic_cast<const service::dns::CloudflareError*>(&error)) {
            return {
                .message = boundedError(cloudflare->what()),
                .permanent =
                    cloudflare->code() == service::dns::CloudflareErrorCode::credentialInvalid ||
                    cloudflare->code() == service::dns::CloudflareErrorCode::authorizationFailed,
            };
        }
        if (const auto* aliyun = dynamic_cast<const service::dns::AliyunError*>(&error)) {
            return {
                .message = boundedError(aliyun->what()),
                .permanent = aliyun->code() == service::dns::AliyunErrorCode::credentialInvalid ||
                             aliyun->code() == service::dns::AliyunErrorCode::authorizationFailed,
            };
        }
        if (const auto* certificate =
                dynamic_cast<const service::certificate_issuance::CertificateProviderClientError*>(
                    &error)) {
            return {
                .message = boundedError(certificate->what()),
                .permanent = !certificate->retryable(),
            };
        }
        if (const auto* verification = dynamic_cast<const VerificationError*>(&error)) {
            return {
                .message = boundedError(verification->what()),
                .permanent = verification->permanent(),
            };
        }
        if (dynamic_cast<const std::invalid_argument*>(&error) != nullptr) {
            return {.message = boundedError(error.what()), .permanent = true};
        }
        return {.message = boundedError(error.what()), .permanent = false};
    } catch (...) {
        return {.message = "供应商检测发生未知错误", .permanent = false};
    }
}

inline ruvia::Task<void> processMarker(service::background::WorkerContext& context,
                                       const VerificationTask& task) {
    std::exception_ptr exception;
    try {
        co_await execute(context, task);
    } catch (...) {
        exception = std::current_exception();
    }
    if (exception) {
        const auto failure = classifyTaskFailure(exception);
        co_await fail(context, task, failure.message, failure.permanent);
    }
    co_return;
}

inline ruvia::Task<void> processMarkers(service::background::WorkerContext& context,
                                        std::size_t& processed) {
    for (; processed < kMaxJobsPerTick; ++processed) {
        const auto marker = co_await claim(context);
        if (!marker) {
            break;
        }
        co_await processMarker(context, *marker);
    }
    co_return;
}

inline ruvia::Task<void> run(service::background::WorkerContext& context) {
    auto nextLeaseRecovery = std::chrono::steady_clock::now();
    auto nextReconciliation = std::chrono::steady_clock::now();
    while (!context.stopToken().stopRequested()) {
        std::string workerError;
        std::size_t processed = 0;
        try {
            if (std::chrono::steady_clock::now() >= nextLeaseRecovery) {
                co_await recoverStaleMarkers(context);
                nextLeaseRecovery = std::chrono::steady_clock::now() + kLeaseRecoveryInterval;
            }
            if (std::chrono::steady_clock::now() >= nextReconciliation) {
                co_await reconcile(context);
                nextReconciliation = std::chrono::steady_clock::now() + kReconciliationInterval;
            }
            co_await processMarkers(context, processed);
        } catch (const std::exception& error) {
            workerError = boundedError(error.what());
        } catch (...) {
            workerError = "未知供应商检测错误";
        }
        if (!workerError.empty()) {
            service::logging::error("Provider verification worker failure: " + workerError);
        }
        const bool shouldIdle = processed == 0 || !workerError.empty();
        if (shouldIdle &&
            co_await ruvia::sleepFor(context.worker(), kIdlePollInterval, context.stopToken()) ==
                ruvia::TimerSleepResult::kStopRequested) {
            break;
        }
    }
    co_return;
}

} // namespace detail

inline ruvia::Task<void> runWorker(service::background::WorkerContext& context) {
    co_await detail::run(context);
}

} // namespace service::provider_verification
