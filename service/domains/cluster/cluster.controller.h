#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <ruvia/core/Task.h>
#include <ruvia/web/Context.h>
#include <ruvia/web/Controller.h>

#include "service/common/http.h"
#include "service/common/types.h"
#include "service/domains/cluster/cluster.schema.h"
#include "service/domains/cluster/cluster.service.h"
#include "service/middleware/auth.h"

namespace service::cluster {

class ClusterController final : public ruvia::Controller<ClusterController> {
  public:
    RUVIA_CONTROLLER_GROUP("/api/clusters", service::middleware::AuthMiddleware)
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/", list);
    RUVIA_POST("/", create, SaveClusterValidator);
    RUVIA_PUT("/:id", update, SaveClusterValidator);
    RUVIA_DELETE("/:id", remove);
    RUVIA_ROUTES_END

  private:
    static std::string requireId(ruvia::Context& c) {
        return service::common::requireUuidParam(c, "id");
    }

    static const std::string& tenantId(ruvia::Context& c) {
        return service::middleware::currentTenantId(c);
    }

    ruvia::Task<ruvia::HttpResponse> list(ruvia::Context& c) {
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
        co_return c.json(service::common::ok<ClusterPageResponse>(
            c, co_await clusterService().list(c, tenantId(c), page, pageSize, skip, keyword,
                                              dnsZoneId, status)));
    }

    ruvia::Task<ruvia::HttpResponse> create(ruvia::Context& c) {
        co_await clusterService().create(c, tenantId(c), c.req().validated<SaveClusterBody>());
        service::common::setRevisionEtag(c, 1);
        co_return c.json(service::common::operation(c, "集群已创建"));
    }

    ruvia::Task<ruvia::HttpResponse> update(ruvia::Context& c) {
        const auto revision = service::common::requireExpectedRevision(c);
        co_await clusterService().update(c, tenantId(c), requireId(c), revision,
                                         c.req().validated<SaveClusterBody>());
        service::common::setRevisionEtag(c, revision + 1);
        co_return c.json(service::common::operation(c, "集群配置已更新"));
    }

    ruvia::Task<ruvia::HttpResponse> remove(ruvia::Context& c) {
        const auto revision = service::common::requireExpectedRevision(c);
        co_await clusterService().remove(c, tenantId(c), requireId(c), revision);
        service::common::setRevisionEtag(c, revision + 1);
        co_return c.json(service::common::operation(c, "集群已删除"));
    }
};

} // namespace service::cluster
