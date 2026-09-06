#pragma once

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

#include "service/features/background/marker_worker_loop.h"
#include "service/features/background/worker_pool.h"
#include "service/features/certificate/acme_account.h"
#include "service/features/certificate/model.h"
#include "service/features/certificate/persistence.h"
#include "service/features/certificate/queue.h"
#include "service/features/certificate/task.h"
#include "service/features/certificate/task_loader.h"
#include "service/features/certificate/work_loader.h"
#include "service/features/dns_sync/queue.h"
#include "service/features/node_dispatch/queue.h"
#include "service/features/sync_runtime/error.h"
#include "service/features/sync_runtime/state.h"
#include "service/utils/secret.h"

namespace service::certificate_issuance {

namespace worker_detail {

inline constexpr std::chrono::seconds kIdlePollInterval{2};
inline constexpr std::chrono::seconds kLeaseRecoveryInterval{15};
inline constexpr std::chrono::minutes kReconciliationInterval{15};
inline constexpr std::size_t kMaxJobsPerTick{4};

inline ruvia::Task<void> recoverStaleMarkers(service::background::WorkerContext& context) {
    co_await service::sync_runtime::recoverStaleRunning(
        context.db(), service::sync_runtime::MarkerResourceType::certificate);
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
                locked.front()[0].as<std::int64_t>().value_or(0) == 0
                    ? service::sync_runtime::MarkerOperation::issue
                    : service::sync_runtime::MarkerOperation::renew);
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
                                            row[1].value().value_or(""), issuanceRevision,
                                            service::sync_runtime::MarkerOperation::renew);
        co_await transaction.commit();
    }
    co_return;
}

inline ruvia::Task<void> execute(service::background::WorkerContext& context,
                                 const CertificateTask& task) {
    const auto lease = service::sync_runtime::makeRunningLease(task.tenantId, task.id, task.version,
                                                               context.leaseOwner());
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
    if (!co_await markIssuanceStarted(context, task, lease)) {
        co_return;
    }

    const DnsChallengeConfigView dnsConfig{.dnsZoneId = work.dnsZoneId,
                                           .zoneDomain = work.zoneDomain,
                                           .certificateId = task.certificateId,
                                           .minimumRecordTtl = work.dnsMinimumRecordTtl};
    auto issued = co_await client.issue(context, account.privateKeyPem.view(), account.accountUrl,
                                        work.domains, dnsConfig, lease);
    co_await persistIssuedCertificate(context, task, lease, issued);
    co_return;
}

inline ruvia::Task<void> processClaimedTask(service::background::WorkerContext& context,
                                            const CertificateTask& task) {
    std::string taskError;
    bool permanent = false;
    try {
        co_await execute(context, task);
    } catch (const AcmeError& error) {
        taskError = service::sync_runtime::boundedError(error.what());
        permanent = error.permanent();
    } catch (const std::exception& error) {
        taskError = service::sync_runtime::boundedError(error.what());
    } catch (...) {
        taskError = "证书任务发生未知错误";
    }
    if (!taskError.empty()) {
        co_await failCertificateTask(context, task, taskError, permanent);
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
    co_await service::background::runMarkerWorkerLoop(
        context, kIdlePollInterval, "Certificate worker failure: ", "未知证书同步错误",
        runMaintenance, processAvailableTasks, service::sync_runtime::boundedError);
    co_return;
}

} // namespace worker_detail

inline ruvia::Task<void> runWorker(service::background::WorkerContext& context) {
    co_await worker_detail::run(context);
}

} // namespace service::certificate_issuance
