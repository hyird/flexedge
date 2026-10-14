#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/context.h>
#include <ruvia/web/controller.h>
#include <ruvia/web/model_json.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/web/request_validation.h"

#include "service/common/http.h"
#include "service/common/types.h"
#include "service/domains/cluster/cluster_command_service.h"
#include "service/domains/cluster/cluster_read_service.h"
#include "service/domains/cluster/cluster_schema.h"
#include "service/features/live_resource/fanout.h"
#include "service/features/live_resource/sse.h"
#include "service/middleware/auth.h"

namespace service::cluster {

class cluster_controller final {
public:
    RUVIA_CONTROLLER_GROUP("/api/clusters", service::middleware::auth_middleware)
    RUVIA_ROUTES_BEGIN(cluster_controller)
    RUVIA_GET_SSE("/stream", list);
    RUVIA_GET_SSE("/options/stream", options);
    RUVIA_GET_SSE("/:id/stream", detail);
    RUVIA_POST("/", create, ruvia::json_body<save_cluster_body>, save_cluster_validator);
    RUVIA_PUT("/:id", update, ruvia::json_body<save_cluster_body>, save_cluster_validator);
    RUVIA_DELETE("/:id", remove);
    RUVIA_ROUTES_END

private:
    ruvia::task<void> options(ruvia::context& c) {
        const auto keyword = service::common::require_keyword(c.req().query("keyword"));
        const auto tenant = tenant_id(c);
        auto sub = service::live_resource::get_hub().subscribe(
            c.worker(), tenant, service::live_resource::resource::clusters, {},
            service::live_resource::query_key("options", keyword.value_or("")));
        co_await service::live_resource::stream_snapshot(
            c, std::move(sub), [tenant, keyword](auto& read) -> ruvia::task<std::string> {
                ruvia::array<cluster_dto> data({.resource_ = read.pool()});
                for (std::int64_t page_number = 1;; ++page_number) {
                    auto page = co_await get_cluster_read_service().list(read, tenant, page_number, 1000,
                        (page_number - 1) * 1000, keyword,
                        std::nullopt, std::nullopt);
                    auto& items = page.template ensure<"list">();
                    for (auto& item : items) {
                        data.push_back(std::move(item));
                    }
                    if (page_number >= page.template get<"total_pages">().value_) {
                        break;
                    }
                }
                co_return std::string(ruvia::to_json(
                    service::common::ok<cluster_options_response>(read, std::move(data)),
                    {.resource_ = read.pool()}));
            });
    }

    static std::string require_id(ruvia::context& c) {
        return service::common::require_uuid_param(c, "id");
    }

    static const std::string& tenant_id(ruvia::context& c) {
        return service::middleware::current_tenant_id(c);
    }

    ruvia::task<void> list(ruvia::context& c) {
        const auto [page, page_size, skip] = service::common::require_pagination(c);
        const auto keyword = service::common::require_keyword(c.req().query("keyword"));
        std::optional<std::string> dns_zone_id;
        if (const auto value = c.req().query("dns_zone_id")) {
            dns_zone_id = service::common::parse_uuid(value);
            if (!dns_zone_id) {
                service::common::throw_app_error(service::common::validation_error_code,
                    "dns_zone_id 必须是 UUID", 400);
            }
        }
        std::optional<std::string> status;
        if (const auto value = c.req().query("status")) {
            if (*value != "enabled" && *value != "disabled") {
                service::common::throw_app_error(service::common::validation_error_code,
                    "status 不正确", 400);
            }
            status.emplace(*value);
        }
        const auto tenant = tenant_id(c);
        auto sub = service::live_resource::get_hub().subscribe(
            c.worker(), tenant, service::live_resource::resource::clusters, {},
            service::live_resource::query_key("list", page, page_size, keyword.value_or(""),
                dns_zone_id, status));
        co_await service::live_resource::stream_snapshot(
            c, std::move(sub),
            [tenant, page, page_size, skip, keyword, dns_zone_id,
                status](auto& read) -> ruvia::task<std::string> {
                auto data = co_await get_cluster_read_service().list(read, tenant, page, page_size, skip,
                    keyword, dns_zone_id, status);
                co_return std::string(
                    ruvia::to_json(service::common::ok<cluster_page_response>(read, std::move(data)),
                        {.resource_ = read.pool()}));
            });
    }

    ruvia::task<void> detail(ruvia::context& c) {
        const auto id = require_id(c);
        const auto tenant = tenant_id(c);
        // Node changes and online/offline transitions publish clusters with
        // an empty id, which also wakes detail subscriptions.
        auto sub = service::live_resource::get_hub().subscribe(
            c.worker(), tenant, service::live_resource::resource::clusters, id,
            service::live_resource::query_key("detail"));
        co_await service::live_resource::stream_snapshot(
            c, std::move(sub), [tenant, id](auto& read) -> ruvia::task<std::string> {
                auto data = co_await get_cluster_read_service().get(read, tenant, id);
                co_return std::string(
                    ruvia::to_json(service::common::ok<cluster_detail_response>(read, std::move(data)),
                        {.resource_ = read.pool()}));
            });
    }

    ruvia::task<ruvia::http_response> create(ruvia::context& c) {
        co_await get_cluster_command_service().create(c, tenant_id(c),
            c.req().validated<save_cluster_body>());
        service::common::set_revision_etag(c, 1);
        co_return c.json(service::common::operation(c, "集群已创建"));
    }

    ruvia::task<ruvia::http_response> update(ruvia::context& c) {
        const auto revision = service::common::require_expected_revision(c);
        co_await get_cluster_command_service().update(c, tenant_id(c), require_id(c), revision,
            c.req().validated<save_cluster_body>());
        service::common::set_revision_etag(c, revision + 1);
        co_return c.json(service::common::operation(c, "集群配置已更新"));
    }

    ruvia::task<ruvia::http_response> remove(ruvia::context& c) {
        const auto revision = service::common::require_expected_revision(c);
        co_await get_cluster_command_service().remove(c, tenant_id(c), require_id(c), revision);
        service::common::set_revision_etag(c, revision + 1);
        co_return c.json(service::common::operation(c, "集群已删除"));
    }
};

}  // namespace service::cluster
