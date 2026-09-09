#pragma once

#include <chrono>
#include <cstddef>
#include <exception>
#include <string>
#include <string_view>

#include <ruvia/core/Task.h>
#include <ruvia/core/Timer.h>

#include "service/features/background/worker_pool.h"
#include "service/features/logging/logger.h"
#include "service/features/sync_runtime/state.h"

namespace service::background {

template <typename Maintenance, typename ProcessAvailable, typename FormatError>
ruvia::Task<void> runMarkerWorkerLoop(WorkerContext& context, std::chrono::seconds idlePollInterval,
                                      std::string_view failureLogPrefix,
                                      std::string_view unknownFailure, Maintenance maintenance,
                                      ProcessAvailable processAvailable, FormatError formatError) {
    auto nextLeaseRecovery = std::chrono::steady_clock::now();
    auto nextReconciliation = std::chrono::steady_clock::now();
    auto nextEventPrune = std::chrono::steady_clock::now();
    while (!context.stopToken().stopRequested()) {
        std::string workerError;
        std::size_t processed = 0;
        try {
            co_await maintenance(context, nextLeaseRecovery, nextReconciliation);
            if (context.stopToken().stopRequested()) {
                break;
            }
            if (std::chrono::steady_clock::now() >= nextEventPrune) {
                co_await service::sync_runtime::pruneResultEvents(context.db());
                nextEventPrune = std::chrono::steady_clock::now() + std::chrono::hours{1};
            }
            if (context.stopToken().stopRequested()) {
                break;
            }
            co_await processAvailable(context, processed);
        } catch (const std::exception& error) {
            workerError = formatError(error.what());
        } catch (...) {
            workerError = std::string(unknownFailure);
        }
        if (context.stopToken().stopRequested()) {
            break;
        }
        if (!workerError.empty()) {
            service::logging::error(std::string(failureLogPrefix) + workerError);
        }
        if ((processed == 0 || !workerError.empty()) &&
            co_await ruvia::sleepFor(context.worker(), idlePollInterval, context.stopToken()) ==
                ruvia::TimerSleepResult::kStopRequested) {
            break;
        }
    }
    co_return;
}

} // namespace service::background
