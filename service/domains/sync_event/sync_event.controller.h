#pragma once

#include <chrono>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <string_view>

#include <ruvia/core/Task.h>
#include <ruvia/web/Context.h>
#include <ruvia/web/Controller.h>
#include <ruvia/web/ModelJson.h>
#include <ruvia/web/Streaming.h>

#include "service/common/http.h"
#include "service/common/types.h"
#include "service/features/sync_event/fanout.h"
#include "service/domains/sync_event/sync_event.service.h"
#include "service/middleware/auth.h"

namespace service::sync_event {

class SyncEventController final : public ruvia::Controller<SyncEventController> {
  public:
    RUVIA_CONTROLLER_GROUP("/api/sync-events", service::middleware::AuthMiddleware)
    RUVIA_ROUTES_BEGIN
    RUVIA_GET_SSE("/stream", stream);
    RUVIA_ROUTES_END

  private:
    static const std::string& tenantId(ruvia::Context& c) {
        return service::middleware::currentTenantId(c);
    }

    static std::optional<std::int64_t> optionalSseCursor(ruvia::Context& c) {
        const auto value =
            c.req().query("after").value_or(c.req().header("Last-Event-ID").value_or(""));
        if (value.empty()) {
            return std::nullopt;
        }
        const auto cursor = service::common::parseInt64(value);
        if (!cursor || *cursor < 0) {
            service::common::throwAppError(service::common::kValidationErrorCode,
                                           "同步事件游标不正确", 400);
        }
        return cursor;
    }

    static bool sseClientDisconnected(const std::exception& error) {
        const std::string_view message{error.what()};
        return message.contains("Broken pipe") || message.contains("Connection reset by peer") ||
               message.contains("redis operation cancelled");
    }

    static bool hasEvents(const SyncEventPageDataDto& data) {
        const auto& items = data.get<"list">();
        return !items.empty();
    }

    static std::int64_t cursorValue(const SyncEventPageDataDto& data) {
        return data.get<"cursor">().value;
    }

    static bool hasMore(const SyncEventPageDataDto& data) { return data.get<"hasMore">().value; }

    static ruvia::Task<void> writeEvent(ruvia::Context& c, ruvia::SseWriter& events,
                                        SyncEventPageDataDto data) {
        const auto cursor = cursorValue(data);
        const auto cursorId = std::to_string(cursor);
        const auto payload =
            ruvia::toJson(service::common::ok<SyncEventPageResponse>(c, std::move(data)),
                          {.resource = c.resource()});
        co_await events.write({.data = payload, .event = "sync-events", .id = cursorId});
    }

    ruvia::Task<void> stream(ruvia::Context& c) {
        try {
            co_await streamBody(c);
        } catch (const std::exception& error) {
            if (sseClientDisconnected(error)) {
                co_return;
            }
            throw;
        }
    }

    static ruvia::Task<void> streamBody(ruvia::Context& c) {
        const auto tenant = tenantId(c);
        auto cursor = optionalSseCursor(c);
        auto subscription = service::sync_event::fanout::hub().subscribe(c.worker(), tenant);
        auto initial = co_await syncEventService().list(c, tenant, cursor);
        cursor = cursorValue(initial);

        auto events = c.streamSse();
        co_await events.write(
            {.data = "{}", .event = "ready", .retry = std::chrono::milliseconds{3000}});
        if (hasEvents(initial)) {
            co_await writeEvent(c, events, std::move(initial));
        }

        while (!events.aborted()) {
            const auto signal = co_await subscription.receiveFor(
                service::sync_event::fanout::kSseHeartbeatInterval, c.stopToken());
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

            while (!events.aborted()) {
                auto update = co_await syncEventService().list(c, tenant, cursor);
                const auto nextCursor = cursorValue(update);
                if (!hasEvents(update) || nextCursor <= *cursor) {
                    break;
                }
                cursor = nextCursor;
                const auto more = hasMore(update);
                co_await writeEvent(c, events, std::move(update));
                if (!more) {
                    break;
                }
            }
        }
    }
};

} // namespace service::sync_event
