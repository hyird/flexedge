#pragma once

#include <chrono>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <utility>

#include <ruvia/core/Task.h>
#include <ruvia/web/Context.h>
#include <ruvia/web/Controller.h>
#include <ruvia/web/ModelJson.h>
#include <ruvia/web/Streaming.h>

#include "service/common/http.h"
#include "service/domains/website/website.schema.h"
#include "service/domains/website/website.service.h"
#include "service/features/log_ingest/fanout.h"
#include "service/features/log_ingest/tail.h"
#include "service/middleware/auth.h"

namespace service::website {

class WebsiteController final : public ruvia::Controller<WebsiteController> {
  public:
    RUVIA_CONTROLLER_GROUP("/api/websites", service::middleware::AuthMiddleware)
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/", list);
    RUVIA_GET_SSE("/:id/access-logs/stream", accessLogStream);
    RUVIA_GET("/:id", detail);
    RUVIA_POST("/", create, WebsiteConfigValidator);
    RUVIA_PUT("/:id", update, WebsiteConfigValidator);
    RUVIA_DELETE("/:id", remove);
    RUVIA_ROUTES_END

  private:
    static std::string requireId(ruvia::Context& c) {
        return service::common::requireUuidParam(c, "id");
    }

    static const std::string& tenantId(ruvia::Context& c) {
        return service::middleware::currentTenantId(c);
    }

    static std::string requireClusterId(ruvia::Context& c) {
        const auto id = service::common::parseUuid(c.req().query("cluster_id"));
        if (!id) {
            service::common::throwAppError(service::common::kValidationErrorCode,
                                           "cluster_id 必须是 UUID", 400);
        }
        return *id;
    }

    static std::int64_t expectedRevision(ruvia::Context& c) {
        return service::common::requireExpectedRevision(c);
    }

    static std::optional<std::string> cursorValue(const WebsiteAccessLogTailDataDto& data) {
        const auto& cursor = data.get<"cursor">();
        if (!cursor) {
            return std::nullopt;
        }
        return std::string(cursor->view());
    }

    static void advanceCursor(std::optional<service::log_ingest::TailCursor>& target,
                              const std::optional<std::string>& cursor) {
        if (!cursor) {
            return;
        }
        if (const auto parsed = service::log_ingest::parseTailCursor(*cursor)) {
            target = *parsed;
        }
    }

    static ruvia::Task<void> writeLogEvent(ruvia::Context& c, ruvia::SseWriter& events,
                                           WebsiteAccessLogTailDataDto data,
                                           const std::optional<std::string>& cursor) {
        auto response = service::common::ok<WebsiteAccessLogTailResponse>(c, std::move(data));
        const auto payload = ruvia::toJson(response, {.resource = c.resource()});
        if (cursor) {
            co_await events.write({.data = payload, .event = "logs", .id = *cursor});
        } else {
            co_await events.write({.data = payload, .event = "logs"});
        }
    }

    ruvia::Task<ruvia::HttpResponse> list(ruvia::Context& c) {
        const auto [page, pageSize, skip] = service::common::requirePagination(c);
        const auto keyword = service::common::requireKeyword(c.req().query("keyword"));
        std::optional<std::string> clusterId;
        if (const auto value = c.req().query("cluster_id")) {
            clusterId = service::common::parseUuid(value);
            if (!clusterId) {
                service::common::throwAppError(service::common::kValidationErrorCode,
                                               "cluster_id 必须是 UUID", 400);
            }
        }
        std::optional<std::string> status;
        if (const auto value = c.req().query("status")) {
            if (*value != "enabled" && *value != "disabled") {
                service::common::throwAppError(service::common::kValidationErrorCode,
                                               "status 不正确", 400);
            }
            status.emplace(*value);
        }
        co_return c.json(service::common::ok<WebsitePageResponse>(
            c, co_await websiteService().list(c, tenantId(c), page, pageSize, skip, keyword,
                                              clusterId, status)));
    }

    ruvia::Task<ruvia::HttpResponse> detail(ruvia::Context& c) {
        auto data = co_await websiteService().detail(c, tenantId(c), requireId(c));
        service::common::setRevisionEtag(c, data.get<"revision">().value);
        co_return c.json(service::common::ok<WebsiteDetailResponse>(c, std::move(data)));
    }

    ruvia::Task<void> accessLogStream(ruvia::Context& c) {
        try {
            co_await accessLogStreamBody(c);
        } catch (const std::exception& error) {
            if (service::log_ingest::sseClientDisconnected(error)) {
                co_return;
            }
            throw;
        }
    }

    static ruvia::Task<void> accessLogStreamBody(ruvia::Context& c) {
        const auto tenant = tenantId(c);
        const auto id = requireId(c);
        const auto limit = service::log_ingest::requireTailLimit(c);
        auto after = service::log_ingest::optionalSseTailCursor(c);
        auto subscription =
            service::log_ingest::fanout::hub().subscribeAccess(c.worker(), tenant, id);
        auto initial = co_await websiteService().accessLogs(c, tenant, id, limit, after);
        const auto initialCursor = cursorValue(initial);
        advanceCursor(after, initialCursor);

        auto events = c.streamSse();
        co_await events.write(
            {.data = "{}", .event = "ready", .retry = std::chrono::milliseconds{3000}});
        co_await writeLogEvent(c, events, std::move(initial), initialCursor);

        while (!events.aborted()) {
            const auto signal = co_await subscription.receiveFor(
                service::log_ingest::fanout::kSseHeartbeatInterval, c.stopToken());
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

            auto update = co_await websiteService().accessLogs(c, tenant, id, limit, after);
            const auto updateCursor = cursorValue(update);
            if (!updateCursor) {
                continue;
            }
            advanceCursor(after, updateCursor);
            co_await writeLogEvent(c, events, std::move(update), updateCursor);
        }
    }

    ruvia::Task<ruvia::HttpResponse> create(ruvia::Context& c) {
        co_await websiteService().create(c, tenantId(c), requireClusterId(c),
                                         c.req().validatedJson<WebsiteSaveInput>());
        service::common::setRevisionEtag(c, 1);
        co_return c.json(service::common::operation(c, "网站已创建，配置任务已提交"));
    }

    ruvia::Task<ruvia::HttpResponse> update(ruvia::Context& c) {
        const auto revision = expectedRevision(c);
        co_await websiteService().update(c, tenantId(c), requireId(c), requireClusterId(c),
                                         revision, c.req().validatedJson<WebsiteSaveInput>());
        service::common::setRevisionEtag(c, revision + 1);
        co_return c.json(service::common::operation(c, "网站配置已更新，配置任务已提交"));
    }

    ruvia::Task<ruvia::HttpResponse> remove(ruvia::Context& c) {
        const auto revision = expectedRevision(c);
        co_await websiteService().remove(c, tenantId(c), requireId(c), revision);
        service::common::setRevisionEtag(c, revision + 1);
        co_return c.json(service::common::operation(c, "网站已删除，配置任务已提交"));
    }
};

} // namespace service::website
