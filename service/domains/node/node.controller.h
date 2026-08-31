#pragma once

#include <chrono>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <ruvia/core/Task.h>
#include <ruvia/web/Context.h>
#include <ruvia/web/Controller.h>
#include <ruvia/web/ModelJson.h>
#include <ruvia/web/Streaming.h>

#include "service/common/http.h"
#include "service/domains/node/node.schema.h"
#include "service/domains/node/node.service.h"
#include "service/features/log_ingest/fanout.h"
#include "service/features/log_ingest/tail.h"
#include "service/middleware/auth.h"

namespace service::node {

class NodeController final : public ruvia::Controller<NodeController> {
  public:
    RUVIA_CONTROLLER_GROUP("/api/nodes", service::middleware::AuthMiddleware)
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/", list);
    RUVIA_POST("/", create, NodeConfigValidator);
    RUVIA_PUT("/:id", update, NodeConfigValidator);
    RUVIA_GET_SSE("/:id/logs/stream", logStream);
    RUVIA_GET("/:id/credentials", credentials);
    RUVIA_POST("/:id/credentials", resetCredentials);
    RUVIA_DELETE("/:id", remove);
    RUVIA_ROUTES_END

  private:
    static std::string requireId(ruvia::Context& c) {
        return service::common::requireUuidParam(c, "id");
    }

    static const std::string& tenantId(ruvia::Context& c) {
        return service::middleware::currentTenantId(c);
    }

    static std::int64_t expectedRevision(ruvia::Context& c) {
        return service::common::requireExpectedRevision(c);
    }

    static std::optional<std::string> cursorValue(const NodeLogTailDataDto& data) {
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
                                           NodeLogTailDataDto data,
                                           const std::optional<std::string>& cursor) {
        auto response = service::common::ok<NodeLogTailResponse>(c, std::move(data));
        const auto payload = ruvia::toJson(response, {.resource = c.resource()});
        if (cursor) {
            co_await events.write({.data = payload, .event = "logs", .id = *cursor});
        } else {
            co_await events.write({.data = payload, .event = "logs"});
        }
    }

    static std::optional<std::string> enumQuery(ruvia::Context& c, std::string_view name,
                                                std::initializer_list<std::string_view> allowed) {
        const auto value = c.req().query(name);
        if (!value) {
            return std::nullopt;
        }
        for (const auto item : allowed) {
            if (*value == item) {
                return std::string(*value);
            }
        }
        service::common::throwAppError(service::common::kValidationErrorCode,
                                       std::string(name) + " 不正确", 400);
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
        const auto status = enumQuery(c, "status", {"enabled", "disabled"});
        const auto registrationStatus =
            enumQuery(c, "registration_status", {"pending", "registered"});
        const auto connectionStatus =
            enumQuery(c, "connection_status", {"unregistered", "online", "offline"});
        co_return c.json(service::common::ok<NodePageResponse>(
            c, co_await nodeService().list(c, tenantId(c), page, pageSize, skip, keyword, clusterId,
                                           status, registrationStatus, connectionStatus)));
    }

    ruvia::Task<ruvia::HttpResponse> create(ruvia::Context& c) {
        c.header("cache-control", "no-store");
        auto data =
            co_await nodeService().create(c, tenantId(c), c.req().validatedJson<NodeSaveInput>());
        service::common::setRevisionEtag(c, data.get<"revision">().value);
        co_return c.json(service::common::ok<NodeCredentialsResponse>(c, std::move(data)));
    }

    ruvia::Task<ruvia::HttpResponse> update(ruvia::Context& c) {
        const auto revision = expectedRevision(c);
        co_await nodeService().update(c, tenantId(c), requireId(c), revision,
                                      c.req().validatedJson<NodeSaveInput>());
        service::common::setRevisionEtag(c, revision + 1);
        co_return c.json(service::common::operation(c, "节点配置已更新"));
    }

    ruvia::Task<ruvia::HttpResponse> remove(ruvia::Context& c) {
        const auto revision = expectedRevision(c);
        co_await nodeService().remove(c, tenantId(c), requireId(c), revision);
        service::common::setRevisionEtag(c, revision + 1);
        co_return c.json(service::common::operation(c, "节点已删除"));
    }

    ruvia::Task<ruvia::HttpResponse> credentials(ruvia::Context& c) {
        c.header("cache-control", "no-store");
        auto data = co_await nodeService().credentials(c, tenantId(c), requireId(c));
        service::common::setRevisionEtag(c, data.get<"revision">().value);
        co_return c.json(service::common::ok<NodeCredentialsResponse>(c, std::move(data)));
    }

    ruvia::Task<ruvia::HttpResponse> resetCredentials(ruvia::Context& c) {
        c.header("cache-control", "no-store");
        const auto revision = expectedRevision(c);
        auto data = co_await nodeService().resetCredentials(c, tenantId(c), requireId(c), revision);
        service::common::setRevisionEtag(c, data.get<"revision">().value);
        co_return c.json(service::common::ok<NodeCredentialsResponse>(c, std::move(data)));
    }

    ruvia::Task<void> logStream(ruvia::Context& c) {
        try {
            co_await logStreamBody(c);
        } catch (const std::exception& error) {
            if (service::log_ingest::sseClientDisconnected(error)) {
                co_return;
            }
            throw;
        }
    }

    static ruvia::Task<void> logStreamBody(ruvia::Context& c) {
        const auto tenant = tenantId(c);
        const auto id = requireId(c);
        const auto limit = service::log_ingest::requireTailLimit(c);
        auto after = service::log_ingest::optionalSseTailCursor(c);
        auto subscription =
            service::log_ingest::fanout::hub().subscribeNode(c.worker(), tenant, id);
        auto initial = co_await nodeService().logs(c, tenant, id, limit, after);
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

            auto update = co_await nodeService().logs(c, tenant, id, limit, after);
            const auto updateCursor = cursorValue(update);
            if (!updateCursor) {
                continue;
            }
            advanceCursor(after, updateCursor);
            co_await writeLogEvent(c, events, std::move(update), updateCursor);
        }
    }
};

} // namespace service::node
