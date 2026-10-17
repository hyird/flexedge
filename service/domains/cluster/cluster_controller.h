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
    RUVIA_GET("/", list);
    RUVIA_GET_SSE("/stream", list_stream);
    RUVIA_GET("/options", options);
    RUVIA_GET("/:id", detail);
    RUVIA_GET_SSE("/:id/stream", detail_stream);
    RUVIA_POST("/", create, ruvia::json_body<save_cluster_body>, save_cluster_validator);
    RUVIA_PUT("/:id", update, ruvia::json_body<save_cluster_body>, save_cluster_validator);
    RUVIA_DELETE("/:id", remove);
    RUVIA_ROUTES_END

private:
    struct list_query final {
        std::int64_t page_{};
        std::int64_t page_size_{};
        std::int64_t skip_{};
        std::optional<std::string> keyword_;
        std::optional<std::string> dns_zone_id_;
        std::optional<std::string> status_;
    };

    static list_query parse_list(ruvia::context& c) {
        const auto [page, page_size, skip] = service::common::require_pagination(c);
        return {.page_ = page,
            .page_size_ = page_size,
            .skip_ = skip,
            .keyword_ = service::common::require_keyword(c.req().query("keyword")),
            .dns_zone_id_ = service::common::optional_uuid_query(c, "dns_zone_id"),
            .status_ = service::common::require_enum_query(c, "status", {"enabled", "disabled"},
                "status 不正确")};
    }

    static ruvia::task<cluster_page_response> read_list(auto& read, const std::string& tenant,
        const list_query& q) {
        auto data = co_await get_cluster_read_service().list(read, tenant, q.page_, q.page_size_, q.skip_,
            q.keyword_, q.dns_zone_id_, q.status_);
        co_return service::common::ok<cluster_page_response>(read, std::move(data));
    }

    static ruvia::task<cluster_detail_response> read_detail(auto& read, const std::string& tenant,
        const std::string& id) {
        auto data = co_await get_cluster_read_service().get(read, tenant, id);
        co_return service::common::ok<cluster_detail_response>(read, std::move(data));
    }

    ruvia::task<ruvia::http_response> options(ruvia::context& c) {
        const auto keyword = service::common::require_keyword(c.req().query("keyword"));
        const auto& tenant = service::middleware::current_tenant_id(c);
        auto data = co_await get_cluster_read_service().options(c, tenant, keyword);
        co_return c.json(service::common::ok<cluster_options_response>(c, std::move(data)));
    }

    ruvia::task<ruvia::http_response> list(ruvia::context& c) {
        co_return c.json(co_await read_list(c, service::middleware::current_tenant_id(c), parse_list(c)));
    }

    ruvia::task<void> list_stream(ruvia::context& c) {
        const auto q = parse_list(c);
        const auto tenant = service::middleware::current_tenant_id(c);
        auto sub = service::live_resource::get_hub(c).subscribe(
            c.worker(), tenant, service::live_resource::resource::clusters, {},
            service::live_resource::query_key("list", q.page_, q.page_size_, q.keyword_.value_or(""),
                q.dns_zone_id_, q.status_));
        co_await service::live_resource::stream_snapshot(
            c, std::move(sub), [tenant, q](auto& read) -> ruvia::task<cluster_page_response> {
                co_return co_await read_list(read, tenant, q);
            });
    }

    ruvia::task<ruvia::http_response> detail(ruvia::context& c) {
        co_return c.json(co_await read_detail(c, service::middleware::current_tenant_id(c),
            service::common::require_uuid_param(c, "id")));
    }

    ruvia::task<void> detail_stream(ruvia::context& c) {
        const auto id = service::common::require_uuid_param(c, "id");
        const auto tenant = service::middleware::current_tenant_id(c);
        // Node changes and online/offline transitions publish clusters with
        // an empty id, which also wakes detail subscriptions.
        auto sub = service::live_resource::get_hub(c).subscribe(
            c.worker(), tenant, service::live_resource::resource::clusters, id,
            service::live_resource::query_key("detail"));
        co_await service::live_resource::stream_snapshot(
            c, std::move(sub), [tenant, id](auto& read) -> ruvia::task<cluster_detail_response> {
                co_return co_await read_detail(read, tenant, id);
            });
    }

    ruvia::task<ruvia::http_response> create(ruvia::context& c) {
        const auto revision = co_await get_cluster_command_service().create(c,
            service::middleware::current_tenant_id(c), c.req().validated<save_cluster_body>());
        service::common::set_revision_etag(c, revision);
        co_return c.json(service::common::operation(c, "集群已创建"));
    }

    ruvia::task<ruvia::http_response> update(ruvia::context& c) {
        const auto expected_revision = service::common::require_expected_revision(c);
        const auto revision = co_await get_cluster_command_service().update(c,
            service::middleware::current_tenant_id(c), service::common::require_uuid_param(c, "id"),
            expected_revision, c.req().validated<save_cluster_body>());
        service::common::set_revision_etag(c, revision);
        co_return c.json(service::common::operation(c, "集群配置已更新"));
    }

    ruvia::task<ruvia::http_response> remove(ruvia::context& c) {
        const auto expected_revision = service::common::require_expected_revision(c);
        const auto revision = co_await get_cluster_command_service().remove(c,
            service::middleware::current_tenant_id(c), service::common::require_uuid_param(c, "id"),
            expected_revision);
        service::common::set_revision_etag(c, revision);
        co_return c.json(service::common::operation(c, "集群已删除"));
    }
};

}  // namespace service::cluster
