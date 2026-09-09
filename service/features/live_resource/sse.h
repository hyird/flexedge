#pragma once

#include <chrono>
#include <exception>
#include <optional>
#include <memory>
#include <string>
#include <ruvia/web/ModelJson.h>
#include <ruvia/web/Streaming.h>
#include "service/common/connection_error.h"
#include "service/common/http.h"
#include "service/features/live_resource/fanout.h"
#include "service/features/live_resource/read_scope.h"
#include "service/features/live_resource/read_snapshot.h"
#include "service/middleware/auth.h"

namespace service::live_resource {

template <typename Fetch>
ruvia::Task<void> streamSnapshot(ruvia::Context& c, Hub::Subscription subscription, Fetch fetch,
                                std::string_view snapshotEvent = "snapshot") {
    // Business errors are SSE frames as well, including errors in the initial
    // snapshot. EventSource cannot inspect HTTP error bodies.
    auto events = c.streamSse();
    const auto credential = std::make_shared<std::optional<service::auth::SessionCredential>>(
        service::auth::readSessionCookie(c));
    auto checkedAt = std::chrono::steady_clock::now();
    // Duplicate suppression must not pin snapshots evicted by the byte budget.
    std::weak_ptr<const Snapshot> lastSnapshot;
    bool initial = true;
    PendingChanges pending{.snapshot = true, .runtime = {}, .origins = {}};
    try {
        co_await events.write(
            {.data = "{}", .event = "ready", .retry = std::chrono::milliseconds(3000)});
        while (!events.aborted()) {
            const auto now = std::chrono::steady_clock::now();
            if (now - checkedAt >= kHeartbeatInterval) {
                // This bounded session check is security validation, never a
                // resource reload. Revoked sessions cannot retain live access.
                const auto principal =
                    co_await readOnce<std::optional<service::auth::AuthenticatedPrincipal>>(
                        c,
                        [credential](ruvia::WebWorkerContext& read)
                            -> ruvia::Task<std::optional<service::auth::AuthenticatedPrincipal>> {
                            if (!*credential)
                                co_return std::nullopt;
                            co_return co_await service::auth::resolveSessionPrincipal(read.db(),
                                                                                      **credential);
                        });
                if (!principal ||
                    principal->system_tenant_id != service::middleware::currentTenantId(c)) {
                    co_await events.write({.data = "{}", .event = "session-expired"});
                    co_return;
                }
                checkedAt = now;
            }
            if (pending.snapshot) {
                std::shared_ptr<const Snapshot> snapshot;
                try {
                    snapshot = co_await readSnapshot(c, subscription.snapshots(), fetch);
                } catch (const SnapshotCapacityError&) {
                    snapshot = std::make_shared<const Snapshot>(snapshotUnavailable());
                }
                if (snapshot->failure) {
                    co_await events.write({.data = snapshot->data, .event = "resource-error"});
                } else {
                    const auto previous = lastSnapshot.lock();
                    if (initial || !previous || snapshot->data != previous->data)
                        co_await events.write({.data = snapshot->data, .event = snapshotEvent});
                    lastSnapshot = snapshot;
                }
                // A shared read may predate patches in this mailbox. Replay
                // them after the baseline; revision/time guards in the client
                // ignore patches older than the fetched lifecycle state.
                initial = snapshot->failure;
            }
            for (const auto& [id, runtime] : pending.runtime) {
                (void)id;
                co_await events.write({.data = runtime, .event = "node-runtime"});
            }
            for (const auto& [id, origins] : pending.origins) {
                (void)id;
                co_await events.write({.data = origins, .event = "origin-runtime"});
            }
            const auto signal = co_await subscription.receiveFor(kHeartbeatInterval, c.stopToken());
            if (events.aborted())
                co_return;
            if (!signal.hasValue()) {
                if (signal.status() != ruvia::WorkerWaitStatus::kTimedOut)
                    co_return;
                co_await events.write({.data = "{}", .event = "heartbeat"});
            }
            pending = subscription.drain();
        }
    } catch (const std::exception& error) {
        if (service::common::sseClientDisconnected(error))
            co_return;
        throw;
    }
}

} // namespace service::live_resource
