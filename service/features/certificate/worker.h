#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ruvia/core/Task.h>
#include <ruvia/core/Timer.h>

#include "service/features/background/worker_pool.h"
#include "service/features/certificate/acme.h"
#include "service/features/certificate_material/model.h"
#include "service/features/certificate/model.h"
#include "service/features/certificate/provider_config.h"
#include "service/features/certificate/queue.h"
#include "service/features/dns/driver.h"
#include "service/features/dns_sync/queue.h"
#include "service/features/node_dispatch/queue.h"
#include "service/features/logging/logger.h"
#include "service/features/sync_runtime/state.h"
#include "service/utils/secret.h"

namespace service::certificate_issuance {

namespace worker_detail {

inline constexpr std::chrono::seconds kIdlePollInterval{2};
inline constexpr std::chrono::seconds kLeaseRecoveryInterval{15};
inline constexpr std::chrono::minutes kReconciliationInterval{15};
inline constexpr std::size_t kMaxJobsPerTick{4};

struct CertificateTask final {
    std::string id;
    std::string tenantId;
    std::string certificateId;
    std::string operation;
    std::int64_t version;
    std::int64_t failures;
};

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

inline std::string boundedError(std::string_view value) {
    constexpr std::size_t limit{1000};
    return std::string(value.substr(0, std::min(value.size(), limit)));
}

inline ruvia::Task<void> recoverStaleMarkers(service::background::WorkerContext& context) {
    co_await service::sync_runtime::recoverStaleRunning(context.db(), "certificate");
    co_return;
}

inline ruvia::Task<void> reconcile(service::background::WorkerContext& context) {
    const auto expired = co_await context.db().query(
        "SELECT tenant_id, id FROM sys_certificate WHERE deleted_at IS NULL AND "
        "issued_revision > 0 AND expires_at <= NOW() AND status <> 'expired' ORDER BY sort ASC");
    for (const auto& row : expired) {
        auto transaction = co_await context.db().beginTransaction();
        const auto updated = co_await transaction.query(
            "UPDATE sys_certificate SET status = 'expired', updated_at = NOW() WHERE tenant_id "
            "= $1 AND id = $2 AND deleted_at IS NULL AND issued_revision > 0 AND expires_at <= "
            "NOW() AND status <> 'expired' RETURNING id",
            row[0].value().value_or(""), row[1].value().value_or(""));
        if (!updated.empty()) {
            co_await service::node_dispatch::enqueueCertificateConsumers(
                transaction, row[0].value().value_or(""), updated.front()[0].value().value_or(""));
        }
        co_await transaction.commit();
    }

    const auto missing = co_await context.db().query(
        "SELECT tenant_id, id, issuance_revision FROM sys_certificate certificate WHERE "
        "certificate.deleted_at IS NULL AND certificate.issued_revision < "
        "certificate.issuance_revision "
        "AND NOT EXISTS (SELECT 1 FROM sys_sync_task marker WHERE marker.resource_type = "
        "'certificate' AND marker.tenant_id = certificate.tenant_id AND marker.resource_id = "
        "certificate.id AND marker.version = certificate.issuance_revision)");
    for (const auto& row : missing) {
        auto transaction = co_await context.db().beginTransaction();
        const auto revision = row[2].as<std::int64_t>().value_or(1);
        const auto locked = co_await transaction.query(
            "SELECT issued_revision FROM sys_certificate WHERE tenant_id = $1 AND id = $2 AND "
            "issuance_revision = $3 AND deleted_at IS NULL LIMIT 1 FOR UPDATE",
            row[0].value().value_or(""), row[1].value().value_or(""), revision);
        if (!locked.empty()) {
            co_await enqueueCertificateRevision(
                transaction, row[0].value().value_or(""), row[1].value().value_or(""), revision,
                locked.front()[0].as<std::int64_t>().value_or(0) == 0 ? std::string_view{"issue"}
                                                                      : std::string_view{"renew"});
        }
        co_await transaction.commit();
    }

    const auto candidates = co_await context.db().query(
        "SELECT tenant_id, id FROM sys_certificate WHERE deleted_at IS NULL AND expires_at "
        "<= NOW() + INTERVAL '30 days' AND status IN ('valid', 'expired') ORDER BY sort ASC");
    for (const auto& row : candidates) {
        auto transaction = co_await context.db().beginTransaction();
        const auto locked = co_await transaction.query(
            "SELECT issuance_revision, config::text FROM sys_certificate WHERE tenant_id = $1 "
            "AND id = $2 AND deleted_at IS NULL AND expires_at <= NOW() + INTERVAL '30 days' AND "
            "status IN ('valid', 'expired') FOR UPDATE",
            row[0].value().value_or(""), row[1].value().value_or(""));
        if (locked.empty()) {
            co_await transaction.commit();
            continue;
        }
        const auto config = parseConfigStored(locked.front()[1].value().value_or("{}"),
                                              {.resource = context.resource()});
        if (!config || !config->autoRenew) {
            co_await transaction.commit();
            continue;
        }
        const auto active = co_await transaction.query(
            "SELECT id FROM sys_sync_task WHERE resource_type = 'certificate' AND tenant_id = $1 "
            "AND resource_id = $2 AND NOT is_done LIMIT 1",
            row[0].value().value_or(""), row[1].value().value_or(""));
        if (!active.empty()) {
            co_await transaction.commit();
            continue;
        }
        const auto issuanceRevision = locked.front()[0].as<std::int64_t>().value_or(0) + 1;
        (void)co_await transaction.execute(
            "UPDATE sys_certificate SET issuance_revision = $1, status = 'renewing', last_error = "
            "NULL, updated_at = NOW() WHERE tenant_id = $2 AND id = $3",
            issuanceRevision, row[0].value().value_or(""), row[1].value().value_or(""));
        co_await enqueueCertificateRevision(transaction, row[0].value().value_or(""),
                                            row[1].value().value_or(""), issuanceRevision, "renew");
        co_await transaction.commit();
    }
    co_return;
}

inline ruvia::Task<std::optional<CertificateTask>>
claim(service::background::WorkerContext& context) {
    const auto rows = co_await context.db().query(
        "WITH candidate AS (SELECT marker.id FROM sys_sync_task marker INNER JOIN sys_certificate "
        "certificate ON certificate.tenant_id = marker.tenant_id AND certificate.id = "
        "marker.resource_id WHERE marker.resource_type = 'certificate' AND NOT marker.is_done AND "
        "marker.next_attempt_at <= NOW() AND marker.lease_until IS NULL AND "
        "certificate.deleted_at IS NULL AND certificate.issuance_revision = marker.version ORDER "
        "BY "
        "marker.next_attempt_at ASC, marker.updated_at ASC FOR UPDATE OF marker SKIP LOCKED LIMIT "
        "1) "
        "UPDATE sys_sync_task marker SET lease_owner = $1, lease_until = NOW() + INTERVAL '60 "
        "seconds', "
        "updated_at = NOW() FROM candidate WHERE marker.id = candidate.id RETURNING marker.id, "
        "marker.tenant_id, marker.resource_id, marker.operation, marker.version, "
        "marker.count_fails",
        context.leaseOwner());
    if (rows.empty()) {
        co_return std::nullopt;
    }
    const auto& row = rows.front();
    co_return CertificateTask{
        .id = std::string(row[0].value().value_or("")),
        .tenantId = std::string(row[1].value().value_or("")),
        .certificateId = std::string(row[2].value().value_or("")),
        .operation = std::string(row[3].value().value_or("")),
        .version = row[4].as<std::int64_t>().value_or(1),
        .failures = row[5].as<std::int64_t>().value_or(0),
    };
}

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

inline ruvia::Task<void> execute(service::background::WorkerContext& context,
                                 const CertificateTask& task) {
    const service::sync_runtime::RunningMarkerLease lease{
        .marker = {.tenantId = task.tenantId, .markerId = task.id, .version = task.version},
        .owner = context.leaseOwner(),
    };
    auto work = co_await loadWork(context, task);
    const auto settings = settingsForProvider(work.provider);
    if (!settings) {
        throw AcmeError("不支持的证书供应商", true);
    }
    AcmeClient client(*settings);
    const auto accountEmail =
        work.accountEmail ? std::optional<std::string_view>{*work.accountEmail} : std::nullopt;
    auto account =
        co_await ensureAcmeAccount(context, work.tenantId, work.providerId, work.providerRevision,
                                   accountEmail, client, work.eab);
    auto started = co_await context.db().beginTransaction();
    const auto startedUpdate = co_await started.execute(
        "UPDATE sys_certificate SET status = CASE WHEN issued_revision = 0 THEN 'issuing' ELSE "
        "'renewing' END, last_error = NULL, updated_at = NOW() WHERE id = $1 AND tenant_id = "
        "$2 AND issuance_revision = $3 AND deleted_at IS NULL",
        task.certificateId, task.tenantId, task.version);
    if (startedUpdate.affectedRows() == 0) {
        (void)co_await service::sync_runtime::releaseRunning(started, lease);
        co_await started.commit();
        co_return;
    }
    if (!co_await service::sync_runtime::renewRunningLease(started, lease)) {
        throw std::runtime_error("证书同步标记 lease 已失效");
    }
    co_await started.commit();

    const DnsChallengeConfigView dnsConfig{.dnsZoneId = work.dnsZoneId,
                                           .zoneDomain = work.zoneDomain,
                                           .certificateId = task.certificateId,
                                           .minimumRecordTtl = work.dnsMinimumRecordTtl};
    auto issued = co_await client.issue(context, account.privateKeyPem.view(), account.accountUrl,
                                        work.domains, dnsConfig, lease);
    const auto nowRows =
        co_await context.db().query("SELECT TO_CHAR(NOW(), 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF')");
    service::certificate_material::CertificateMaterialOutput material(
        {.resource = context.resource()});
    material.set<"certificateChainPem">(issued.certificateChainPem);
    material.set<"privateKeyEnvelope">(service::utils::sealSecret(issued.privateKeyPem.view()));
    material.set<"notBefore">(issued.notBefore);
    material.set<"serialNumber">(issued.serialNumber);
    material.set<"fingerprintSha256">(issued.fingerprintSha256);
    material.set<"lastIssuedAt">(nowRows.front()[0].value().value_or(""));
    const auto materialJson = ruvia::toJson(material, {.resource = context.resource()});

    auto transaction = co_await context.db().beginTransaction();
    const auto updated = co_await transaction.execute(
        "UPDATE sys_certificate SET material = $1::jsonb, expires_at = $2::timestamptz, "
        "issued_revision = $3, status = 'valid', last_error = NULL, updated_at = NOW() WHERE id = "
        "$4 AND tenant_id = $5 AND issuance_revision = $3 AND deleted_at IS NULL",
        std::string_view(materialJson), std::string_view(issued.expiresAt), task.version,
        task.certificateId, task.tenantId);
    if (updated.affectedRows() != 0) {
        if (!co_await service::sync_runtime::renewRunningLease(transaction, lease)) {
            throw std::runtime_error("证书同步标记 lease 已失效");
        }
        co_await service::node_dispatch::enqueueCertificateConsumers(transaction, task.tenantId,
                                                                     task.certificateId);
        if (!co_await service::sync_runtime::completeRunning(transaction, lease)) {
            throw std::runtime_error("证书同步标记 lease 已失效");
        }
    } else {
        (void)co_await service::sync_runtime::releaseRunning(transaction, lease);
    }
    co_await transaction.commit();
    co_return;
}

inline ruvia::Task<void> fail(service::background::WorkerContext& context,
                              const CertificateTask& task, std::string_view error, bool permanent) {
    const auto message = boundedError(error);
    const service::sync_runtime::RunningMarkerLease lease{
        .marker = {.tenantId = task.tenantId, .markerId = task.id, .version = task.version},
        .owner = context.leaseOwner(),
    };
    auto transaction = co_await context.db().beginTransaction();
    (void)co_await transaction.query(
        "SELECT id FROM sys_certificate WHERE tenant_id = $1 AND id = $2 LIMIT 1 FOR UPDATE",
        task.tenantId, task.certificateId);
    const bool transitioned =
        co_await service::sync_runtime::failRunning(transaction, lease, message);
    if (transitioned) {
        const auto updated = co_await transaction.query(
            "UPDATE sys_certificate SET status = CASE WHEN expires_at > NOW() THEN CASE WHEN $3 "
            "THEN 'valid' ELSE 'renewing' END WHEN $3 THEN 'failed' ELSE 'pending' END, "
            "last_error = $4, updated_at = NOW() WHERE id = $1 AND tenant_id = $5 AND "
            "issuance_revision = $2 AND deleted_at IS NULL RETURNING issued_revision > 0 AND "
            "expires_at <= NOW()",
            task.certificateId, task.version, permanent, std::string_view(message), task.tenantId);
        if (!updated.empty() && updated.front()[0].as<bool>().value_or(false)) {
            co_await service::node_dispatch::enqueueCertificateConsumers(transaction, task.tenantId,
                                                                         task.certificateId);
        }
    }
    co_await transaction.commit();
    if (!transitioned) {
        co_return;
    }
    service::logging::error("Certificate sync marker " + task.id + " failed: " + message);
    co_return;
}

inline ruvia::Task<void> processClaimedTask(service::background::WorkerContext& context,
                                            const CertificateTask& task) {
    std::string taskError;
    bool permanent = false;
    try {
        co_await execute(context, task);
    } catch (const AcmeError& error) {
        taskError = boundedError(error.what());
        permanent = error.permanent();
    } catch (const std::exception& error) {
        taskError = boundedError(error.what());
    } catch (...) {
        taskError = "证书任务发生未知错误";
    }
    if (!taskError.empty()) {
        co_await fail(context, task, taskError, permanent);
    }
    co_return;
}

inline ruvia::Task<void> processAvailableTasks(service::background::WorkerContext& context,
                                               std::size_t& processed) {
    for (; processed < kMaxJobsPerTick; ++processed) {
        const auto task = co_await claim(context);
        if (!task) {
            break;
        }
        co_await processClaimedTask(context, *task);
    }
    co_return;
}

inline ruvia::Task<void> runMaintenance(service::background::WorkerContext& context,
                                        std::chrono::steady_clock::time_point& nextLeaseRecovery,
                                        std::chrono::steady_clock::time_point& nextReconciliation) {
    if (std::chrono::steady_clock::now() >= nextLeaseRecovery) {
        co_await recoverStaleMarkers(context);
        nextLeaseRecovery = std::chrono::steady_clock::now() + kLeaseRecoveryInterval;
    }
    if (std::chrono::steady_clock::now() >= nextReconciliation) {
        co_await reconcile(context);
        nextReconciliation = std::chrono::steady_clock::now() + kReconciliationInterval;
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
            co_await runMaintenance(context, nextLeaseRecovery, nextReconciliation);
            co_await processAvailableTasks(context, processed);
        } catch (const std::exception& error) {
            workerError = boundedError(error.what());
        } catch (...) {
            workerError = "未知证书同步错误";
        }
        if (!workerError.empty()) {
            service::logging::error("Certificate worker failure: " + workerError);
        }
        const auto delay =
            processed == 0 || !workerError.empty() ? kIdlePollInterval : std::chrono::seconds{0};
        if (delay.count() > 0 &&
            co_await ruvia::sleepFor(context.worker(), delay, context.stopToken()) ==
                ruvia::TimerSleepResult::kStopRequested) {
            break;
        }
    }
    co_return;
}

} // namespace worker_detail

inline ruvia::Task<void> runWorker(service::background::WorkerContext& context) {
    co_await worker_detail::run(context);
}

} // namespace service::certificate_issuance
