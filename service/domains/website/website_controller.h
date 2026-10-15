#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/context.h>
#include <ruvia/web/controller.h>
#include <ruvia/web/model_json.h>
#include <ruvia/web/streaming.h>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "service/common/http.h"
#include "service/domains/website/website_access_log_service.h"
#include "service/domains/website/website_command_service.h"
#include "service/domains/website/website_dashboard_service.h"
#include "service/domains/website/website_filter.h"
#include "service/domains/website/website_read_service.h"
#include "service/domains/website/website_schema.h"
#include "service/features/live_resource/fanout.h"
#include "service/features/live_resource/sse.h"
#include "service/features/log_ingest/fanout.h"
#include "service/features/log_ingest/sse_tail.h"
#include "service/features/log_ingest/tail.h"
#include "service/middleware/auth.h"

namespace service::website {

class website_controller final {
public:
    RUVIA_CONTROLLER_GROUP("/api/websites", service::middleware::auth_middleware)
    RUVIA_ROUTES_BEGIN(website_controller)
    RUVIA_GET_SSE("/stream", list);
    RUVIA_GET_SSE("/:id/access-logs/history/stream", access_log_history);
    RUVIA_GET_SSE("/:id/access-logs/stream", access_log_stream);
    RUVIA_GET_SSE("/:id/dashboard/stream", dashboard_stream);
    RUVIA_GET_SSE("/:id/stream", detail);
    RUVIA_POST("/:id/dns-probe", request_dns_probe);
    RUVIA_POST("/", create, ruvia::json_body<website_save_input>, website_config_validator);
    RUVIA_PUT("/:id", update, ruvia::json_body<website_save_input>, website_config_validator);
    RUVIA_DELETE("/:id", remove);
    RUVIA_ROUTES_END

private:
    static std::string require_id(ruvia::context& c) {
        return service::common::require_uuid_param(c, "id");
    }

    static const std::string& tenant_id(ruvia::context& c) {
        return service::middleware::current_tenant_id(c);
    }

    static std::string require_cluster_id(ruvia::context& c) {
        const auto id = service::common::parse_uuid(c.req().query("cluster_id"));
        if (!id) {
            service::common::throw_app_error(service::common::validation_error_code,
                "cluster_id 必须是 UUID", 400);
        }
        return *id;
    }

    static std::int64_t expected_revision(ruvia::context& c) {
        return service::common::require_expected_revision(c);
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
        std::optional<std::string> status;
        if (const auto value = c.req().query("status")) {
            if (*value != "enabled" && *value != "disabled") {
                service::common::throw_app_error(service::common::validation_error_code,
                    "status 不正确", 400);
            }
            status.emplace(*value);
        }
        const auto certificate_id = parse_certificate_id_filter(c.req().query("certificate_id"));
        const auto tenant = tenant_id(c);
        auto sub = service::live_resource::get_hub(c).subscribe(c.worker(), tenant,
            service::live_resource::resource::websites, {},
            service::live_resource::query_key("list", page, page_size, keyword.value_or(""),
                cluster_id, status, certificate_id));
        co_await service::live_resource::stream_snapshot(
            c, std::move(sub), [tenant, page, page_size, skip, keyword, cluster_id, status, certificate_id](auto& read) -> ruvia::task<std::string> {
                auto data = co_await get_website_read_service().list(read, tenant, page, page_size, skip, keyword, cluster_id, status, certificate_id);
                co_return std::string(ruvia::to_json(
                    service::common::ok<website_page_response>(read, std::move(data)),
                    {.resource_ = read.pool()}));
            });
    }

    ruvia::task<void> detail(ruvia::context& c) {
        const auto id = require_id(c);
        const auto tenant = tenant_id(c);
        auto sub = service::live_resource::get_hub(c).subscribe(c.worker(), tenant,
            service::live_resource::resource::websites, id,
            service::live_resource::query_key("detail"));
        co_await service::live_resource::stream_snapshot(
            c, std::move(sub), [tenant, id](auto& read) -> ruvia::task<std::string> {
                auto data = co_await get_website_read_service().detail(read, tenant, id);
                co_return std::string(ruvia::to_json(
                    service::common::ok<website_detail_response>(read, std::move(data)),
                    {.resource_ = read.pool()}));
            });
    }

    ruvia::task<ruvia::http_response> request_dns_probe(ruvia::context& c) {
        const auto operation_id =
            co_await get_website_command_service().request_dns_probe(c, tenant_id(c), require_id(c));
        co_return c.json(service::common::operation(c, "域名解析检测已提交", operation_id));
    }

    ruvia::task<void> access_log_history(ruvia::context& c) {
        const auto [page, page_size, skip] = service::common::require_pagination(c);
        const auto keyword = service::common::require_keyword(c.req().query("keyword"));
        std::optional<std::string> method;
        if (const auto value = c.req().query("method")) {
            if (*value != "GET" && *value != "POST" && *value != "PUT" && *value != "PATCH" &&
                *value != "DELETE" && *value != "HEAD" && *value != "OPTIONS") {
                service::common::throw_app_error(service::common::validation_error_code,
                    "method 不正确", 400);
            }
            method.emplace(*value);
        }
        std::optional<std::string> status_class;
        if (const auto value = c.req().query("status_class")) {
            if (*value != "1xx" && *value != "2xx" && *value != "3xx" && *value != "4xx" &&
                *value != "5xx") {
                service::common::throw_app_error(service::common::validation_error_code,
                    "status_class 不正确", 400);
            }
            status_class.emplace(*value);
        }
        const auto id = require_id(c);
        const auto tenant = tenant_id(c);
        auto sub = service::live_resource::get_hub(c).subscribe(c.worker(), tenant,
            service::live_resource::resource::access_history, id,
            service::live_resource::query_key("history", page, page_size, keyword.value_or(""),
                method, status_class));
        co_await service::live_resource::stream_snapshot(
            c, std::move(sub), [tenant, id, page, page_size, skip, keyword, method, status_class](auto& read) -> ruvia::task<std::string> {
                auto data = co_await get_website_access_log_service().history(read, tenant, id, page, page_size, skip, keyword, method, status_class);
                co_return std::string(ruvia::to_json(
                    service::common::ok<website_access_log_page_response>(read, std::move(data)),
                    {.resource_ = read.pool()}));
            });
    }

    ruvia::task<void> access_log_stream(ruvia::context& c) {
        const auto tenant = tenant_id(c);
        const auto id = require_id(c);
        const auto limit = service::log_ingest::require_tail_limit(c);
        co_await service::log_ingest::stream_sse_tail(
            c,
            service::log_ingest::fanout::get_hub(c).subscribe(
                c.worker(), service::log_ingest::notifications::log_resource_type::access, tenant,
                id),
            service::log_ingest::optional_sse_tail_cursor(c),
            [tenant, id, limit](auto& read, const std::optional<service::log_ingest::tail_cursor>& after)
                -> ruvia::task<service::log_ingest::tail_batch> {
                auto data = co_await get_website_access_log_service().tail(read, tenant, id, limit, after);
                auto cursor = service::log_ingest::tail_response_cursor(data);
                auto json = ruvia::to_json(service::common::ok<website_access_log_tail_response>(read, std::move(data)),
                    {.resource_ = read.pool()});
                co_return service::log_ingest::tail_batch{.payload_ = std::string(json.data(), json.size()),
                    .cursor_ = std::move(cursor)};
            });
    }

    ruvia::task<void> dashboard_stream(ruvia::context& c) {
        const auto tenant = tenant_id(c);
        const auto id = require_id(c);
        auto sub = service::live_resource::get_hub(c).subscribe(
            c.worker(), tenant, service::live_resource::resource::access_history, id,
            service::live_resource::query_key("dashboard"));
        co_await service::live_resource::stream_snapshot(
            c, std::move(sub),
            [tenant, id](auto& read) -> ruvia::task<std::string> {
                auto data = co_await get_website_dashboard_service().dashboard(read, tenant, id);
                auto json = ruvia::to_json(service::common::ok<website_dashboard_response>(read, std::move(data)),
                    {.resource_ = read.pool()});
                co_return std::string(json.data(), json.size());
            },
            "dashboard");
    }

    ruvia::task<ruvia::http_response> create(ruvia::context& c) {
        auto data = co_await get_website_command_service().create(
            c, tenant_id(c), require_cluster_id(c), c.req().validated_json<website_save_input>());
        service::common::set_revision_etag(c, data.get<"revision">().value_);
        website_created_response response({.resource_ = c.pool()});
        response.set<"code">(0);
        response.set<"message">("网站已创建，配置任务已提交");
        response.set<"data">(std::move(data));
        co_return c.json(response);
    }

    ruvia::task<ruvia::http_response> update(ruvia::context& c) {
        const auto revision = expected_revision(c);
        const auto operation_id = co_await get_website_command_service().update(c, tenant_id(c), require_id(c),
            require_cluster_id(c), revision, c.req().validated_json<website_save_input>());
        service::common::set_revision_etag(c, revision + 1);
        co_return c.json(service::common::operation(c, "网站配置已更新，配置任务已提交", operation_id));
    }

    ruvia::task<ruvia::http_response> remove(ruvia::context& c) {
        const auto revision = expected_revision(c);
        const auto operation_id =
            co_await get_website_command_service().remove(c, tenant_id(c), require_id(c), revision);
        service::common::set_revision_etag(c, revision + 1);
        co_return c.json(service::common::operation(c, "网站已删除，配置任务已提交", operation_id));
    }
};

}  // namespace service::website
