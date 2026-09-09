#pragma once

#include <chrono>
#include <cstddef>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <ruvia/core/Task.h>
#include <ruvia/core/Timer.h>

#include "service/features/background/marker_worker_loop.h"
#include "service/features/background/worker_pool.h"
#include "service/features/certificate/acme_account.h"
#include "service/features/certificate/maintenance.h"
#include "service/features/certificate/persistence.h"
#include "service/features/certificate/task.h"
#include "service/features/certificate/task_loader.h"
#include "service/features/certificate/work_loader.h"
#include "service/features/sync_runtime/error.h"
#include "service/features/sync_runtime/state.h"

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

inline ruvia::Task<void> execute(service::background::WorkerContext& context,
                                 const CertificateTask& task) {
    const auto lease = service::sync_runtime::makeRunningLease(
        task.tenantId, task.id, task.version, context.leaseOwner(),
        service::sync_runtime::MarkerResourceType::certificate, task.certificateId);
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
        co_await reconcileCertificateMaintenance(context);
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
