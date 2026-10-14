#pragma once
#include <ruvia/core/task.h>
#include <ruvia/web/context.h>
#include <ruvia/web/controller.h>
#include <ruvia/web/model_json.h>
#include <ruvia/web/streaming.h>

#include <chrono>
#include <cstdint>
#include <exception>
#include <initializer_list>
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
    RUVIA_CONTROLLER_GROUP("/api/nodes", service::middleware::auth_middleware)
    RUVIA_ROUTES_BEGIN(node_controller)
    RUVIA_GET_SSE("/stream", list);
    RUVIA_GET_SSE("/:id/stream", detail);
    RUVIA_POST("/", create, ruvia::json_body<node_save_input>);
    RUVIA_PUT("/:id", update, ruvia::json_body<node_save_input>);
    RUVIA_GET_SSE("/:id/logs/stream", log_stream);
    RUVIA_POST("/:id/credentials/reveal", credentials);
    RUVIA_POST("/:id/credentials", reset_credentials);
    RUVIA_DELETE("/:id", remove);
    RUVIA_ROUTES_END

private:
    static std::string require_id(ruvia::context& c) {
        return service::common::require_uuid_param(c, "id");
    }

    static const std::string& tenant_id(ruvia::context& c) {
        return service::middleware::current_tenant_id(c);
    }

    static std::int64_t expected_revision(ruvia::context& c) {
        return service::common::require_expected_revision(c);
    }

    static std::optional<std::string> enum_query(ruvia::context& c, std::string_view name,
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
        service::common::throw_app_error(service::common::validation_error_code,
            std::string(name) + " 不正确", 400);
    }

    ruvia::task<void> list(ruvia::context& c) {
        const auto [page, page_size, skip] = service::common::require_pagination(c);
        const auto keyword = service::common::require_keyword(c.req().query("keyword"));
        std::optional<std::string> cluster_id;
        if (const auto value = c.req().query("cluster_id")) {
            cluster_id = service::common::parse_uuid(value);
            if (!cluster_id) {
                service::common::throw_app_error(service::common::validation_error_code,
                    "cluster_id 必须是 UUID", 400);
            }
        }
        const auto status = enum_query(c, "status", {"enabled", "disabled"});
        const auto registration_status =
            enum_query(c, "registration_status", {"pending", "registered"});
        const auto connection_status =
            enum_query(c, "connection_status", {"unregistered", "online", "offline"});
        const auto tenant = tenant_id(c);
        auto sub = service::live_resource::get_hub().subscribe(c.worker(), tenant,
            service::live_resource::resource::nodes, {},
            node_list_query_key(page, page_size, keyword.value_or(""), cluster_id, status,
                registration_status, connection_status));
        co_await service::live_resource::stream_snapshot(c, std::move(sub),
            [tenant, page, page_size, skip, keyword = keyword.value_or(""), cluster_id, status,
                registration_status, connection_status](auto& read) -> ruvia::task<std::string> {
                auto data = co_await get_node_read_service().list(read, tenant, page, page_size, skip,
                    keyword, cluster_id, status, registration_status, connection_status);
                co_return std::string(ruvia::to_json(service::common::ok<node_page_response>(read,
                                                         std::move(data)),
                    {.resource_ = read.pool()}));
            });
    }

    ruvia::task<void> detail(ruvia::context& c) {
        const auto id = require_id(c);
        const auto tenant = tenant_id(c);
        auto sub = service::live_resource::get_hub().subscribe(c.worker(), tenant,
            service::live_resource::resource::nodes, id,
            service::live_resource::query_key("detail"));
        co_await service::live_resource::stream_snapshot(c, std::move(sub),
            [tenant, id](auto& read) -> ruvia::task<std::string> {
                auto data = co_await get_node_read_service().get(read, tenant, id);
                co_return std::string(ruvia::to_json(service::common::ok<node_detail_response>(read,
                                                         std::move(data)),
                    {.resource_ = read.pool()}));
            });
    }

    ruvia::task<ruvia::http_response> create(ruvia::context& c) {
        c.header("cache-control", "no-store");
        const auto body = c.req().validated_json<node_save_input>();
        ruvia::validator validator({.resource_ = c.arena()});
        validate_node_save_input(body.value(), validator);
        validator.throw_if_invalid();
        auto data = co_await get_node_command_service().create(c, tenant_id(c), body);
        service::common::set_revision_etag(c, data.get<"revision">().value_);
        co_return c.json(service::common::ok<node_credentials_response>(c, std::move(data)));
    }

    ruvia::task<ruvia::http_response> update(ruvia::context& c) {
        const auto revision = expected_revision(c);
        const auto body = c.req().validated_json<node_save_input>();
        ruvia::validator validator({.resource_ = c.arena()});
        validate_node_save_input(body.value(), validator);
        validator.throw_if_invalid();
        co_await get_node_command_service().update(c, tenant_id(c), require_id(c), revision, body);
        service::common::set_revision_etag(c, revision + 1);
        co_return c.json(service::common::operation(c, "节点配置已更新"));
    }

    ruvia::task<ruvia::http_response> remove(ruvia::context& c) {
        const auto revision = expected_revision(c);
        co_await get_node_command_service().remove(c, tenant_id(c), require_id(c), revision);
        service::common::set_revision_etag(c, revision + 1);
        co_return c.json(service::common::operation(c, "节点已删除"));
    }

    ruvia::task<ruvia::http_response> credentials(ruvia::context& c) {
        c.header("cache-control", "no-store");
        auto data = co_await get_node_read_service().credentials(c, tenant_id(c), require_id(c));
        service::common::set_revision_etag(c, data.get<"revision">().value_);
        co_return c.json(service::common::ok<node_credentials_response>(c, std::move(data)));
    }

    ruvia::task<ruvia::http_response> reset_credentials(ruvia::context& c) {
        c.header("cache-control", "no-store");
        const auto revision = expected_revision(c);
        auto data =
            co_await get_node_command_service().reset_credentials(c, tenant_id(c), require_id(c), revision);
        service::common::set_revision_etag(c, data.get<"revision">().value_);
        co_return c.json(service::common::ok<node_credentials_response>(c, std::move(data)));
    }

    ruvia::task<void> log_stream(ruvia::context& c) {
        const auto tenant = tenant_id(c);
        const auto id = require_id(c);
        const auto limit = service::log_ingest::require_tail_limit(c);
        co_await service::log_ingest::stream_sse_tail(
            c,
            service::log_ingest::fanout::get_hub().subscribe(
                c.worker(), service::log_ingest::notifications::log_resource_type::node, tenant, id),
            service::log_ingest::optional_sse_tail_cursor(c),
            [tenant, id, limit](auto& read, const std::optional<service::log_ingest::tail_cursor>& after)
                -> ruvia::task<service::log_ingest::tail_batch> {
                auto data = co_await get_node_read_service().logs(read, tenant, id, limit, after);
                auto cursor = service::log_ingest::tail_response_cursor(data);
                auto json = ruvia::to_json(service::common::ok<node_log_tail_response>(read, std::move(data)),
                    {.resource_ = read.pool()});
                co_return service::log_ingest::tail_batch{.payload_ = std::string(json.data(), json.size()),
                    .cursor_ = std::move(cursor)};
            });
    }
};

}  // namespace service::node
