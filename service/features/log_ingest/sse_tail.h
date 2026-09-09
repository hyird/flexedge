#pragma once

#include <chrono>
#include <exception>
#include <optional>
#include <memory>
#include <utility>
#include <string>
#include <string_view>

#include <ruvia/core/Task.h>
#include <ruvia/web/Context.h>
#include <ruvia/web/Streaming.h>

#include "service/common/connection_error.h"
#include "service/features/log_ingest/fanout.h"
#include "service/features/log_ingest/tail.h"
#include "service/features/live_resource/read_scope.h"
#include "service/domains/auth/session_credential.h"
#include "service/domains/auth/auth_session_read.service.h"
#include "service/middleware/auth.h"

namespace service::log_ingest {

struct TailBatch final {
    std::string payload;
    std::optional<std::string> cursor;
};

template <typename TailData>
inline std::optional<std::string> tailResponseCursor(const TailData& data) {
    const auto& cursor = data.template get<"cursor">();
    if (!cursor) {
        return std::nullopt;
    }
    return std::string(cursor->view());
}

inline void advanceTailCursor(std::optional<TailCursor>& target,
                              const std::optional<std::string>& cursor) {
    if (!cursor) {
        return;
    }
    if (const auto parsed = parseTailCursor(*cursor)) {
        target = *parsed;
    }
}

template <typename Fetch>
ruvia::Task<void> streamSseTail(ruvia::Context& c, fanout::Hub::Subscription subscription,
                                std::optional<TailCursor> after, Fetch fetch) {
    try {
        auto events = c.streamSse();
        auto credential = std::make_shared<std::optional<service::auth::SessionCredential>>(
            service::auth::readSessionCookie(c));
        auto checkedAt = std::chrono::steady_clock::now();
        auto initial = co_await service::live_resource::readOnce<TailBatch>(c,
            [fetch, after](auto& read) { return fetch(read, after); });
        const auto initialCursor = initial.cursor;
        advanceTailCursor(after, initialCursor);

        co_await events.write(
            {.data = "{}", .event = "ready", .retry = std::chrono::milliseconds{3000}});
        ruvia::SseMessage initialMessage{.data = initial.payload, .event = "logs"};
        if (initialCursor) {
            initialMessage.id = *initialCursor;
        }
        co_await events.write(initialMessage);

        while (!events.aborted()) {
            if (std::chrono::steady_clock::now() - checkedAt >= fanout::kSseHeartbeatInterval) {
                const auto principal = co_await service::live_resource::readOnce<std::optional<service::auth::AuthenticatedPrincipal>>(
                    c, [credential](auto& read) -> ruvia::Task<std::optional<service::auth::AuthenticatedPrincipal>> {
                        if (!*credential) co_return std::nullopt;
                        co_return co_await service::auth::resolveSessionPrincipal(read.db(), **credential);
                    });
                if (!principal || principal->system_tenant_id != service::middleware::currentTenantId(c)) {
                    co_await events.write({.data = "{}", .event = "session-expired"});
                    co_return;
                }
                checkedAt = std::chrono::steady_clock::now();
            }
            const auto signal =
                co_await subscription.receiveFor(fanout::kSseHeartbeatInterval, c.stopToken());
            if (events.aborted()) {
                co_return;
            }
            if (!signal.hasValue()) {
                if (signal.status() != ruvia::WorkerWaitStatus::kTimedOut) {
                    co_return;
                }
                co_await events.write({.event = "heartbeat"});
                continue;
            }

            auto update = co_await service::live_resource::readOnce<TailBatch>(c,
                [fetch, after](auto& read) { return fetch(read, after); });
            const auto updateCursor = update.cursor;
            if (!updateCursor) {
                continue;
            }
            advanceTailCursor(after, updateCursor);
            co_await events.write({.data = update.payload, .event = "logs", .id = *updateCursor});
        }
    } catch (const std::exception& error) {
        if (service::common::sseClientDisconnected(error)) {
            co_return;
        }
        throw;
    }
}

} // namespace service::log_ingest
