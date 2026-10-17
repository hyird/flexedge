#pragma once
#include <ruvia/core/task.h>
#include <ruvia/web/context.h>
#include <ruvia/web/controller.h>
#include <ruvia/web/model_json.h>
#include <ruvia/web/streaming.h>

#include <chrono>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "service/common/connection_error.h"
#include "service/common/http.h"
#include "service/domains/node/node_command_service.h"
#include "service/domains/node/node_read_service.h"
#include "service/features/live_resource/fanout.h"
#include "service/features/live_resource/sse.h"
#include "service/features/log_ingest/fanout.h"
#include "service/features/log_ingest/sse_tail.h"
#include "service/features/log_ingest/tail.h"
#include "service/middleware/auth.h"

namespace service::node {

class node_controller final {
public:
    using node_save_validator =
        service::common::json_validator_middleware<node_save_input, validate_node_save_input>;

    RUVIA_CONTROLLER_GROUP("/api/nodes", service::middleware::auth_middleware)
    RUVIA_ROUTES_BEGIN(node_controller)
    RUVIA_GET("/", list);
    RUVIA_GET_SSE("/stream", list_stream);
    RUVIA_GET("/:id", detail);
    RUVIA_GET_SSE("/:id/stream", detail_stream);
    RUVIA_POST("/", create, ruvia::json_body<node_save_input>, node_save_validator);
    RUVIA_PUT("/:id", update, ruvia::json_body<node_save_input>, node_save_validator);
    RUVIA_GET_SSE("/:id/logs/stream", log_stream);
    RUVIA_POST("/:id/credentials/reveal", credentials);
    RUVIA_POST("/:id/credentials", reset_credentials);
    RUVIA_DELETE("/:id", remove);
    RUVIA_ROUTES_END

private:
    struct list_query final {
        std::int64_t page_{};
        std::int64_t page_size_{};
        std::int64_t skip_{};
        std::string keyword_;
        std::optional<std::string> cluster_id_;
        std::optional<std::string> status_;
        std::optional<std::string> registration_status_;
        std::optional<std::string> connection_status_;
    };

    static list_query parse_list(ruvia::context& c) {
        const auto [page, page_size, skip] = service::common::require_pagination(c);
        return {.page_ = page,
            .page_size_ = page_size,
            .skip_ = skip,
            .keyword_ = service::common::require_keyword(c.req().query("keyword")).value_or(""),
            .cluster_id_ = service::common::optional_uuid_query(c, "cluster_id"),
            .status_ = service::common::require_enum_query(c, "status", {"enabled", "disabled"},
                "status 不正确"),
            .registration_status_ = service::common::require_enum_query(c, "registration_status",
                {"pending", "registered"}, "registration_status 不正确"),
            .connection_status_ = service::common::require_enum_query(c, "connection_status",
                {"unregistered", "online", "offline"}, "connection_status 不正确")};
    }

    static ruvia::task<node_page_response> read_list(auto& read, const std::string& tenant,
        const list_query& q) {
        auto data = co_await get_node_read_service().list(read, tenant, q.page_, q.page_size_, q.skip_,
            q.keyword_, q.cluster_id_, q.status_, q.registration_status_, q.connection_status_);
        co_return service::common::ok<node_page_response>(read, std::move(data));
    }

    static ruvia::task<node_detail_response> read_detail(auto& read, const std::string& tenant,
        const std::string& id) {
        auto data = co_await get_node_read_service().get(read, tenant, id);
        co_return service::common::ok<node_detail_response>(read, std::move(data));
    }

    ruvia::task<ruvia::http_response> list(ruvia::context& c) {
        co_return c.json(co_await read_list(c, service::middleware::current_tenant_id(c), parse_list(c)));
    }

    ruvia::task<void> list_stream(ruvia::context& c) {
        const auto q = parse_list(c);
        const auto tenant = service::middleware::current_tenant_id(c);
        auto sub = service::live_resource::get_hub(c).subscribe(c.worker(), tenant,
            service::live_resource::resource::nodes, {},
            node_list_query_key(q.page_, q.page_size_, q.keyword_, q.cluster_id_, q.status_,
                q.registration_status_, q.connection_status_));
        co_await service::live_resource::stream_snapshot(c, std::move(sub),
            [tenant, q](auto& read) -> ruvia::task<node_page_response> {
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
        auto sub = service::live_resource::get_hub(c).subscribe(c.worker(), tenant,
            service::live_resource::resource::nodes, id,
            service::live_resource::query_key("detail"));
        co_await service::live_resource::stream_snapshot(c, std::move(sub),
            [tenant, id](auto& read) -> ruvia::task<node_detail_response> {
                co_return co_await read_detail(read, tenant, id);
            });
    }

    ruvia::task<ruvia::http_response> create(ruvia::context& c) {
        c.header("cache-control", "no-store");
        auto data = co_await get_node_command_service().create(
            c, service::middleware::current_tenant_id(c), c.req().validated<node_save_input>());
        service::common::set_revision_etag(c, data.get<"revision">().value_);
        co_return c.json(service::common::ok<node_credentials_response>(c, std::move(data)));
    }

    ruvia::task<ruvia::http_response> update(ruvia::context& c) {
        const auto expected_revision = service::common::require_expected_revision(c);
        const auto revision = co_await get_node_command_service().update(c,
            service::middleware::current_tenant_id(c), service::common::require_uuid_param(c, "id"),
            expected_revision, c.req().validated<node_save_input>());
        service::common::set_revision_etag(c, revision);
        co_return c.json(service::common::operation(c, "节点配置已更新"));
    }

    ruvia::task<ruvia::http_response> remove(ruvia::context& c) {
        const auto expected_revision = service::common::require_expected_revision(c);
        const auto revision = co_await get_node_command_service().remove(c,
            service::middleware::current_tenant_id(c), service::common::require_uuid_param(c, "id"),
            expected_revision);
        service::common::set_revision_etag(c, revision);
        co_return c.json(service::common::operation(c, "节点已删除"));
    }

    ruvia::task<ruvia::http_response> credentials(ruvia::context& c) {
        c.header("cache-control", "no-store");
        auto data = co_await get_node_read_service().credentials(c, service::middleware::current_tenant_id(c),
            service::common::require_uuid_param(c, "id"));
        service::common::set_revision_etag(c, data.get<"revision">().value_);
        co_return c.json(service::common::ok<node_credentials_response>(c, std::move(data)));
    }

    ruvia::task<ruvia::http_response> reset_credentials(ruvia::context& c) {
        c.header("cache-control", "no-store");
        const auto revision = service::common::require_expected_revision(c);
        auto data = co_await get_node_command_service().reset_credentials(c,
            service::middleware::current_tenant_id(c), service::common::require_uuid_param(c, "id"), revision);
        service::common::set_revision_etag(c, data.get<"revision">().value_);
        co_return c.json(service::common::ok<node_credentials_response>(c, std::move(data)));
    }

    ruvia::task<void> log_stream(ruvia::context& c) {
        const auto tenant = service::middleware::current_tenant_id(c);
        const auto id = service::common::require_uuid_param(c, "id");
        const auto limit = service::log_ingest::require_tail_limit(c);
        co_await service::log_ingest::stream_model_sse_tail<node_log_tail_response>(
            c,
            service::log_ingest::fanout::get_hub(c).subscribe(
                c.worker(), service::log_ingest::notifications::log_resource_type::node, tenant, id),
            service::log_ingest::optional_sse_tail_cursor(c),
            [tenant, id, limit](auto& read, const std::optional<service::log_ingest::tail_cursor>& after)
                -> ruvia::task<node_log_tail_data_dto> {
                co_return co_await get_node_read_service().logs(read, tenant, id, limit, after);
            });
    }
};

}  // namespace service::node
