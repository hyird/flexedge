#pragma once

#include <cstdint>
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
#include "service/domains/website/website_access_log.service.h"
#include "service/domains/website/website_command.service.h"
#include "service/domains/website/website_dashboard.service.h"
#include "service/domains/website/website_read.service.h"
#include "service/features/log_ingest/fanout.h"
#include "service/features/log_ingest/sse_tail.h"
#include "service/features/log_ingest/tail.h"
#include "service/middleware/auth.h"
#include "service/features/live_resource/fanout.h"
#include "service/features/live_resource/sse.h"

namespace service::website {

class WebsiteController final : public ruvia::Controller<WebsiteController> {
  public:
    RUVIA_CONTROLLER_GROUP("/api/websites", service::middleware::AuthMiddleware)
    RUVIA_ROUTES_BEGIN
    RUVIA_GET_SSE("/stream", list);
    RUVIA_GET_SSE("/:id/access-logs/history/stream", accessLogHistory);
    RUVIA_GET_SSE("/:id/access-logs/stream", accessLogStream);
    RUVIA_GET_SSE("/:id/dashboard/stream", dashboardStream);
    RUVIA_GET_SSE("/:id/stream", detail);
    RUVIA_POST("/:id/dns-probe", requestDnsProbe);
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
        std::optional<std::string> status;
        if (const auto value = c.req().query("status")) {
            if (*value != "enabled" && *value != "disabled") {
                service::common::throwAppError(service::common::kValidationErrorCode,
                                               "status 不正确", 400);
            }
            status.emplace(*value);
        }
        const auto tenant = tenantId(c);
        auto sub = service::live_resource::hub().subscribe(c.worker(), tenant,
            service::live_resource::Resource::websites, {},
            service::live_resource::queryKey("list", page, pageSize, keyword.value_or(""),
                                             clusterId, status));
        co_await service::live_resource::streamSnapshot(
            c, std::move(sub), [tenant, page, pageSize, skip, keyword, clusterId, status](ruvia::WebWorkerContext& read) -> ruvia::Task<std::string> {
                auto data = co_await websiteReadService().list(read, tenant, page, pageSize, skip, keyword, clusterId, status);
                co_return std::string(ruvia::toJson(
                    service::common::ok<WebsitePageResponse>(read, std::move(data)),
                    {.resource = read.resource()}));
            });
    }

    ruvia::Task<void> detail(ruvia::Context& c) {
        const auto id = requireId(c);
        const auto tenant = tenantId(c);
        auto sub = service::live_resource::hub().subscribe(c.worker(), tenant,
            service::live_resource::Resource::websites, id,
            service::live_resource::queryKey("detail"));
        co_await service::live_resource::streamSnapshot(
            c, std::move(sub), [tenant, id](ruvia::WebWorkerContext& read) -> ruvia::Task<std::string> {
                auto data = co_await websiteReadService().detail(read, tenant, id);
                co_return std::string(ruvia::toJson(
                    service::common::ok<WebsiteDetailResponse>(read, std::move(data)),
                    {.resource = read.resource()}));
            });
    }

    ruvia::Task<ruvia::HttpResponse> requestDnsProbe(ruvia::Context& c) {
        co_await websiteCommandService().requestDnsProbe(c, tenantId(c), requireId(c));
        co_return c.json(service::common::operation(c, "域名解析检测已提交"));
    }

    ruvia::Task<void> accessLogHistory(ruvia::Context& c) {
        const auto [page, pageSize, skip] = service::common::requirePagination(c);
        const auto keyword = service::common::requireKeyword(c.req().query("keyword"));
        std::optional<std::string> method;
        if (const auto value = c.req().query("method")) {
            if (*value != "GET" && *value != "POST" && *value != "PUT" && *value != "PATCH" &&
                *value != "DELETE" && *value != "HEAD" && *value != "OPTIONS") {
                service::common::throwAppError(service::common::kValidationErrorCode,
                                               "method 不正确", 400);
            }
            method.emplace(*value);
        }
        std::optional<std::string> statusClass;
        if (const auto value = c.req().query("status_class")) {
            if (*value != "1xx" && *value != "2xx" && *value != "3xx" && *value != "4xx" &&
                *value != "5xx") {
                service::common::throwAppError(service::common::kValidationErrorCode,
                                               "status_class 不正确", 400);
            }
            statusClass.emplace(*value);
        }
        const auto id = requireId(c);
        const auto tenant = tenantId(c);
        auto sub = service::live_resource::hub().subscribe(c.worker(), tenant,
            service::live_resource::Resource::accessHistory, id,
            service::live_resource::queryKey("history", page, pageSize, keyword.value_or(""),
                                             method, statusClass));
        co_await service::live_resource::streamSnapshot(
            c, std::move(sub), [tenant, id, page, pageSize, skip, keyword, method, statusClass](ruvia::WebWorkerContext& read) -> ruvia::Task<std::string> {
                auto data = co_await websiteAccessLogService().history(read, tenant, id, page, pageSize, skip, keyword, method, statusClass);
                co_return std::string(ruvia::toJson(
                    service::common::ok<WebsiteAccessLogPageResponse>(read, std::move(data)),
                    {.resource = read.resource()}));
            });
    }

    ruvia::Task<void> accessLogStream(ruvia::Context& c) {
        const auto tenant = tenantId(c);
        const auto id = requireId(c);
        const auto limit = service::log_ingest::requireTailLimit(c);
        co_await service::log_ingest::streamSseTail(
            c,
            service::log_ingest::fanout::hub().subscribe(
                c.worker(), service::log_ingest::notifications::LogResourceType::access, tenant,
                id),
            service::log_ingest::optionalSseTailCursor(c),
            [tenant, id, limit](auto& read, const std::optional<service::log_ingest::TailCursor>& after)
                -> ruvia::Task<service::log_ingest::TailBatch> {
                auto data = co_await websiteAccessLogService().tail(read, tenant, id, limit, after);
                auto cursor = service::log_ingest::tailResponseCursor(data);
                auto json = ruvia::toJson(service::common::ok<WebsiteAccessLogTailResponse>(read, std::move(data)),
                                          {.resource = read.resource()});
                co_return service::log_ingest::TailBatch{.payload = std::string(json.data(), json.size()),
                                                         .cursor = std::move(cursor)};
            });
    }

    ruvia::Task<void> dashboardStream(ruvia::Context& c) {
        const auto tenant = tenantId(c);
        const auto id = requireId(c);
        auto sub = service::live_resource::hub().subscribe(
            c.worker(), tenant, service::live_resource::Resource::accessHistory, id,
            service::live_resource::queryKey("dashboard"));
        co_await service::live_resource::streamSnapshot(
            c, std::move(sub),
            [tenant, id](auto& read) -> ruvia::Task<std::string> {
                auto data = co_await websiteDashboardService().dashboard(read, tenant, id);
                auto json = ruvia::toJson(service::common::ok<WebsiteDashboardResponse>(read, std::move(data)),
                                          {.resource = read.resource()});
                co_return std::string(json.data(), json.size());
            }, "dashboard");
    }

    ruvia::Task<ruvia::HttpResponse> create(ruvia::Context& c) {
        co_await websiteCommandService().create(c, tenantId(c), requireClusterId(c),
                                                c.req().validatedJson<WebsiteSaveInput>());
        service::common::setRevisionEtag(c, 1);
        co_return c.json(service::common::operation(c, "网站已创建，配置任务已提交"));
    }

    ruvia::Task<ruvia::HttpResponse> update(ruvia::Context& c) {
        const auto revision = expectedRevision(c);
        co_await websiteCommandService().update(c, tenantId(c), requireId(c), requireClusterId(c),
                                                revision,
                                                c.req().validatedJson<WebsiteSaveInput>());
        service::common::setRevisionEtag(c, revision + 1);
        co_return c.json(service::common::operation(c, "网站配置已更新，配置任务已提交"));
    }

    ruvia::Task<ruvia::HttpResponse> remove(ruvia::Context& c) {
        const auto revision = expectedRevision(c);
        co_await websiteCommandService().remove(c, tenantId(c), requireId(c), revision);
        service::common::setRevisionEtag(c, revision + 1);
        co_return c.json(service::common::operation(c, "网站已删除，配置任务已提交"));
    }
};

} // namespace service::website
