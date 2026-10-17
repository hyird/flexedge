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
#include <vector>

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
    RUVIA_GET("/", list);
    RUVIA_GET_SSE("/stream", list_stream);
    RUVIA_GET("/:id/access-logs/history", access_log_history);
    RUVIA_GET_SSE("/:id/access-logs/stream", access_log_stream);
    RUVIA_GET_SSE("/:id/dashboard/stream", dashboard_stream);
    RUVIA_GET("/:id", detail);
    RUVIA_GET_SSE("/:id/stream", detail_stream);
    RUVIA_POST("/:id/dns-probe", request_dns_probe);
    RUVIA_POST("/", create, ruvia::json_body<website_save_input>, website_config_validator);
    RUVIA_PUT("/:id", update, ruvia::json_body<website_save_input>, website_config_validator);
    RUVIA_DELETE("/:id", remove);
    RUVIA_ROUTES_END

private:
    static std::string require_cluster_id(ruvia::context& c) {
        return service::common::require_uuid_query(c, "cluster_id");
    }

    struct list_query final {
        std::int64_t page_{};
        std::int64_t page_size_{};
        std::int64_t skip_{};
        std::optional<std::string> keyword_;
        std::optional<std::string> cluster_id_;
        std::optional<std::string> status_;
        std::optional<std::string> certificate_id_;
    };

    static list_query parse_list(ruvia::context& c) {
        const auto [page, page_size, skip] = service::common::require_pagination(c);
        return {.page_ = page,
            .page_size_ = page_size,
            .skip_ = skip,
            .keyword_ = service::common::require_keyword(c.req().query("keyword")),
            .cluster_id_ = service::common::optional_uuid_query(c, "cluster_id"),
            .status_ = service::common::require_enum_query(c, "status", {"enabled", "disabled"},
                "status 不正确"),
            .certificate_id_ = parse_certificate_id_filter(c)};
    }

    static ruvia::task<website_page_response> read_list(auto& read, const std::string& tenant,
        const list_query& q) {
        auto data = co_await get_website_read_service().list(read, tenant, q.page_, q.page_size_, q.skip_,
            q.keyword_, q.cluster_id_, q.status_, q.certificate_id_);
        co_return service::common::ok<website_page_response>(read, std::move(data));
    }

    static ruvia::task<website_detail_response> read_detail(auto& read, const std::string& tenant,
        const std::string& id) {
        auto data = co_await get_website_read_service().detail(read, tenant, id);
        co_return service::common::ok<website_detail_response>(read, std::move(data));
    }

    static ruvia::task<website_dashboard_response> read_dashboard(auto& read,
        const std::string& tenant, const std::string& id) {
        auto data = co_await get_website_dashboard_service().dashboard(read, tenant, id);
        co_return service::common::ok<website_dashboard_response>(read, std::move(data));
    }

    ruvia::task<ruvia::http_response> list(ruvia::context& c) {
        co_return c.json(co_await read_list(c, service::middleware::current_tenant_id(c), parse_list(c)));
    }

    ruvia::task<void> list_stream(ruvia::context& c) {
        const auto q = parse_list(c);
        const auto tenant = service::middleware::current_tenant_id(c);
        auto sub = service::live_resource::get_hub(c).subscribe(c.worker(), tenant,
            service::live_resource::resource::websites, {},
            service::live_resource::query_key("list", q.page_, q.page_size_, q.keyword_.value_or(""),
                q.cluster_id_, q.status_, q.certificate_id_));
        co_await service::live_resource::stream_snapshot(
            c, std::move(sub), [tenant, q](auto& read) -> ruvia::task<website_page_response> {
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
            service::live_resource::resource::websites, id,
            service::live_resource::query_key("detail"));
        co_await service::live_resource::stream_snapshot(
            c, std::move(sub), [tenant, id](auto& read) -> ruvia::task<website_detail_response> {
                co_return co_await read_detail(read, tenant, id);
            });
    }

    ruvia::task<ruvia::http_response> request_dns_probe(ruvia::context& c) {
        co_await get_website_command_service().request_dns_probe(c, service::middleware::current_tenant_id(c),
            service::common::require_uuid_param(c, "id"));
        co_return c.json(service::common::operation(c, "域名解析检测已提交"));
    }

    ruvia::task<ruvia::http_response> access_log_history(ruvia::context& c) {
        const auto [page, page_size, skip] = service::common::require_pagination(c);
        const auto keyword = service::common::require_keyword(c.req().query("keyword"));
        const auto method = service::common::require_enum_query(c, "method",
            {"GET", "POST", "PUT", "PATCH", "DELETE", "HEAD", "OPTIONS"}, "method 不正确");
        const auto status_class = service::common::require_enum_query(c, "status_class",
            {"1xx", "2xx", "3xx", "4xx", "5xx"}, "status_class 不正确");
        auto data = co_await get_website_access_log_service().history(c,
            service::middleware::current_tenant_id(c), service::common::require_uuid_param(c, "id"), page,
            page_size, skip, keyword, method, status_class);
        co_return c.json(service::common::ok<website_access_log_page_response>(c, std::move(data)));
    }

    ruvia::task<void> access_log_stream(ruvia::context& c) {
        const auto tenant = service::middleware::current_tenant_id(c);
        const auto id = service::common::require_uuid_param(c, "id");
        const auto limit = service::log_ingest::require_tail_limit(c);
        co_await service::log_ingest::stream_model_sse_tail<website_access_log_tail_response>(
            c,
            service::log_ingest::fanout::get_hub(c).subscribe(
                c.worker(), service::log_ingest::notifications::log_resource_type::access, tenant,
                id),
            service::log_ingest::optional_sse_tail_cursor(c),
            [tenant, id, limit](auto& read, const std::optional<service::log_ingest::tail_cursor>& after)
                -> ruvia::task<website_access_log_tail_data_dto> {
                co_return co_await get_website_access_log_service().tail(read, tenant, id, limit, after);
            });
    }

    ruvia::task<void> dashboard_stream(ruvia::context& c) {
        namespace live = service::live_resource;
        const auto tenant = service::middleware::current_tenant_id(c);
        const auto id = service::common::require_uuid_param(c, "id");
        std::vector<live::subscription_part> parts;
        parts.push_back({live::resource::websites, id, live::query_key("detail")});
        parts.push_back({live::resource::access_history, id, live::query_key("dashboard")});
        auto sub = live::get_hub(c).subscribe(c.worker(), tenant, std::move(parts));
        std::vector<live::snapshot_part> snapshots;
        snapshots.push_back(live::make_snapshot_part("website",
            [tenant, id](auto& read) -> ruvia::task<website_detail_response> {
                co_return co_await read_detail(read, tenant, id);
            }));
        snapshots.push_back(live::make_snapshot_part("dashboard",
            [tenant, id](auto& read) -> ruvia::task<website_dashboard_response> {
                co_return co_await read_dashboard(read, tenant, id);
            }));
        co_await live::stream_snapshots(c, std::move(sub), std::move(snapshots));
    }

    ruvia::task<ruvia::http_response> create(ruvia::context& c) {
        auto data = co_await get_website_command_service().create(
            c, service::middleware::current_tenant_id(c), require_cluster_id(c),
            c.req().validated<website_save_input>());
        service::common::set_revision_etag(c, data.get<"revision">().value_);
        website_created_response response({.resource_ = c.pool()});
        response.set<"code">(0);
        response.set<"message">("网站已创建，配置任务已提交");
        response.set<"data">(std::move(data));
        co_return c.json(response);
    }

    ruvia::task<ruvia::http_response> update(ruvia::context& c) {
        const auto expected_revision = service::common::require_expected_revision(c);
        const auto revision = co_await get_website_command_service().update(c,
            service::middleware::current_tenant_id(c), service::common::require_uuid_param(c, "id"),
            require_cluster_id(c), expected_revision,
            c.req().validated<website_save_input>());
        service::common::set_revision_etag(c, revision);
        co_return c.json(service::common::operation(c, "网站配置已更新，配置任务已提交"));
    }

    ruvia::task<ruvia::http_response> remove(ruvia::context& c) {
        const auto expected_revision = service::common::require_expected_revision(c);
        const auto revision = co_await get_website_command_service().remove(c,
            service::middleware::current_tenant_id(c), service::common::require_uuid_param(c, "id"),
            expected_revision);
        service::common::set_revision_etag(c, revision);
        co_return c.json(service::common::operation(c, "网站已删除，配置任务已提交"));
    }
};

}  // namespace service::website
