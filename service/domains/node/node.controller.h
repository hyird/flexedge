#pragma once
#include "service/common/connection_error.h"

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
#include "service/domains/node/node_command.service.h"
#include "service/domains/node/node_read.service.h"
#include "service/features/log_ingest/fanout.h"
#include "service/features/log_ingest/sse_tail.h"
#include "service/features/log_ingest/tail.h"
#include "service/middleware/auth.h"
#include "service/features/live_resource/fanout.h"
#include "service/features/live_resource/sse.h"

namespace service::node {

class NodeController final : public ruvia::Controller<NodeController> {
  public:
    RUVIA_CONTROLLER_GROUP("/api/nodes", service::middleware::AuthMiddleware)
    RUVIA_ROUTES_BEGIN
    RUVIA_GET_SSE("/stream", list);
    RUVIA_POST("/", create, NodeConfigValidator);
    RUVIA_PUT("/:id", update, NodeConfigValidator);
    RUVIA_GET_SSE("/:id/logs/stream", logStream);
    RUVIA_POST("/:id/credentials/reveal", credentials);
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

    ruvia::Task<void> list(ruvia::Context& c) {
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
        const auto tenant = tenantId(c);
        auto sub = service::live_resource::hub().subscribe(c.worker(), tenant,
            service::live_resource::Resource::nodes, {},
            service::live_resource::queryKey("list", page, pageSize, keyword.value_or(""),
                                             clusterId, status, registrationStatus,
                                             connectionStatus));
        co_await service::live_resource::streamSnapshot(c, std::move(sub),
            [tenant, page, pageSize, skip, keyword = keyword.value_or(""), clusterId, status,
             registrationStatus, connectionStatus](auto& read) -> ruvia::Task<std::string> {
                auto data = co_await nodeReadService().list(read, tenant, page, pageSize, skip,
                    keyword, clusterId, status, registrationStatus, connectionStatus);
                co_return std::string(ruvia::toJson(service::common::ok<NodePageResponse>(read,
                    std::move(data)), {.resource = read.resource()}));
            });
    }

    ruvia::Task<ruvia::HttpResponse> create(ruvia::Context& c) {
        c.header("cache-control", "no-store");
        auto data = co_await nodeCommandService().create(c, tenantId(c),
                                                         c.req().validatedJson<NodeSaveInput>());
        service::common::setRevisionEtag(c, data.get<"revision">().value);
        co_return c.json(service::common::ok<NodeCredentialsResponse>(c, std::move(data)));
    }

    ruvia::Task<ruvia::HttpResponse> update(ruvia::Context& c) {
        const auto revision = expectedRevision(c);
        co_await nodeCommandService().update(c, tenantId(c), requireId(c), revision,
                                             c.req().validatedJson<NodeSaveInput>());
        service::common::setRevisionEtag(c, revision + 1);
        co_return c.json(service::common::operation(c, "节点配置已更新"));
    }

    ruvia::Task<ruvia::HttpResponse> remove(ruvia::Context& c) {
        const auto revision = expectedRevision(c);
        co_await nodeCommandService().remove(c, tenantId(c), requireId(c), revision);
        service::common::setRevisionEtag(c, revision + 1);
        co_return c.json(service::common::operation(c, "节点已删除"));
    }

    ruvia::Task<ruvia::HttpResponse> credentials(ruvia::Context& c) {
        c.header("cache-control", "no-store");
        auto data = co_await nodeReadService().credentials(c, tenantId(c), requireId(c));
        service::common::setRevisionEtag(c, data.get<"revision">().value);
        co_return c.json(service::common::ok<NodeCredentialsResponse>(c, std::move(data)));
    }

    ruvia::Task<ruvia::HttpResponse> resetCredentials(ruvia::Context& c) {
        c.header("cache-control", "no-store");
        const auto revision = expectedRevision(c);
        auto data =
            co_await nodeCommandService().resetCredentials(c, tenantId(c), requireId(c), revision);
        service::common::setRevisionEtag(c, data.get<"revision">().value);
        co_return c.json(service::common::ok<NodeCredentialsResponse>(c, std::move(data)));
    }


    ruvia::Task<void> logStream(ruvia::Context& c) {
        const auto tenant = tenantId(c);
        const auto id = requireId(c);
        const auto limit = service::log_ingest::requireTailLimit(c);
        co_await service::log_ingest::streamSseTail(
            c,
            service::log_ingest::fanout::hub().subscribe(
                c.worker(), service::log_ingest::notifications::LogResourceType::node, tenant, id),
            service::log_ingest::optionalSseTailCursor(c),
            [tenant, id, limit](auto& read, const std::optional<service::log_ingest::TailCursor>& after)
                -> ruvia::Task<service::log_ingest::TailBatch> {
                auto data = co_await nodeReadService().logs(read, tenant, id, limit, after);
                auto cursor = service::log_ingest::tailResponseCursor(data);
                auto json = ruvia::toJson(service::common::ok<NodeLogTailResponse>(read, std::move(data)),
                                          {.resource = read.resource()});
                co_return service::log_ingest::TailBatch{.payload = std::string(json.data(), json.size()),
                                                         .cursor = std::move(cursor)};
            });
    }
};

} // namespace service::node
