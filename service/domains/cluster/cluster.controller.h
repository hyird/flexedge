#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <ruvia/core/Task.h>
#include <ruvia/web/Context.h>
#include <ruvia/web/ModelJson.h>
#include <ruvia/web/Controller.h>

#include "service/common/http.h"
#include "service/common/types.h"
#include "service/domains/cluster/cluster.schema.h"
#include "service/domains/cluster/cluster_command.service.h"
#include "service/domains/cluster/cluster_read.service.h"
#include "service/middleware/auth.h"
#include "service/features/live_resource/fanout.h"
#include "service/features/live_resource/sse.h"

namespace service::cluster {

class ClusterController final : public ruvia::Controller<ClusterController> {
  public:
    RUVIA_CONTROLLER_GROUP("/api/clusters", service::middleware::AuthMiddleware)
    RUVIA_ROUTES_BEGIN
    RUVIA_GET_SSE("/stream", list);
    RUVIA_GET_SSE("/options/stream", options);
    RUVIA_POST("/", create, SaveClusterValidator);
    RUVIA_PUT("/:id", update, SaveClusterValidator);
    RUVIA_DELETE("/:id", remove);
    RUVIA_ROUTES_END

  private:
    ruvia::Task<void> options(ruvia::Context& c) {
        const auto keyword = service::common::requireKeyword(c.req().query("keyword"));
        const auto tenant = tenantId(c);
        auto sub = service::live_resource::hub().subscribe(
            c.worker(), tenant, service::live_resource::Resource::clusters, {},
            service::live_resource::queryKey("options", keyword.value_or("")));
        co_await service::live_resource::streamSnapshot(
            c, std::move(sub), [tenant, keyword](auto& read) -> ruvia::Task<std::string> {
                ruvia::Array<ClusterDto> data(read.resource());
                for (std::int64_t pageNumber = 1;; ++pageNumber) {
                    auto page = co_await clusterReadService().list(read, tenant, pageNumber, 1000,
                                                                   (pageNumber - 1) * 1000, keyword,
                                                                   std::nullopt, std::nullopt);
                    auto& items = page.template ensure<"list">();
                    for (auto& item : items)
                        data.push_back(std::move(item));
                    if (pageNumber >= page.template get<"totalPages">().value)
                        break;
                }
                co_return std::string(ruvia::toJson(
                    service::common::ok<ClusterOptionsResponse>(read, std::move(data)),
                    {.resource = read.resource()}));
            });
    }

    static std::string requireId(ruvia::Context& c) {
        return service::common::requireUuidParam(c, "id");
    }

    static const std::string& tenantId(ruvia::Context& c) {
        return service::middleware::currentTenantId(c);
    }

    ruvia::Task<void> list(ruvia::Context& c) {
        const auto [page, pageSize, skip] = service::common::requirePagination(c);
        const auto keyword = service::common::requireKeyword(c.req().query("keyword"));
        std::optional<std::string> dnsZoneId;
        if (const auto value = c.req().query("dns_zone_id")) {
            dnsZoneId = service::common::parseUuid(value);
            if (!dnsZoneId) {
                service::common::throwAppError(service::common::kValidationErrorCode,
                                               "dns_zone_id 必须是 UUID", 400);
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
        auto sub = service::live_resource::hub().subscribe(
            c.worker(), tenant, service::live_resource::Resource::clusters, {},
            service::live_resource::queryKey("list", page, pageSize, keyword.value_or(""),
                                             dnsZoneId, status));
        co_await service::live_resource::streamSnapshot(
            c, std::move(sub),
            [tenant, page, pageSize, skip, keyword, dnsZoneId,
             status](auto& read) -> ruvia::Task<std::string> {
                auto data = co_await clusterReadService().list(read, tenant, page, pageSize, skip,
                                                               keyword, dnsZoneId, status);
                co_return std::string(
                    ruvia::toJson(service::common::ok<ClusterPageResponse>(read, std::move(data)),
                                  {.resource = read.resource()}));
            });
    }

    ruvia::Task<ruvia::HttpResponse> create(ruvia::Context& c) {
        co_await clusterCommandService().create(c, tenantId(c),
                                                c.req().validated<SaveClusterBody>());
        service::common::setRevisionEtag(c, 1);
        co_return c.json(service::common::operation(c, "集群已创建"));
    }

    ruvia::Task<ruvia::HttpResponse> update(ruvia::Context& c) {
        const auto revision = service::common::requireExpectedRevision(c);
        co_await clusterCommandService().update(c, tenantId(c), requireId(c), revision,
                                                c.req().validated<SaveClusterBody>());
        service::common::setRevisionEtag(c, revision + 1);
        co_return c.json(service::common::operation(c, "集群配置已更新"));
    }

    ruvia::Task<ruvia::HttpResponse> remove(ruvia::Context& c) {
        const auto revision = service::common::requireExpectedRevision(c);
        co_await clusterCommandService().remove(c, tenantId(c), requireId(c), revision);
        service::common::setRevisionEtag(c, revision + 1);
        co_return c.json(service::common::operation(c, "集群已删除"));
    }
};

} // namespace service::cluster
