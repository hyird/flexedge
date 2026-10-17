#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/context.h>
#include <ruvia/web/controller.h>
#include <ruvia/web/model_json.h>
#include <ruvia/web/request_validation.h>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "service/common/domain_name.h"
#include "service/common/http.h"
#include "service/domains/dns_zone/dns_zone_command_service.h"
#include "service/domains/dns_zone/dns_zone_read_service.h"
#include "service/domains/dns_zone/dns_zone_schema.h"
#include "service/features/live_resource/fanout.h"
#include "service/features/live_resource/sse.h"
#include "service/middleware/auth.h"

namespace service::dns_zone {

class dns_zone_controller final {
public:
    RUVIA_CONTROLLER_GROUP("/api/dns-zones", service::middleware::auth_middleware)
    RUVIA_ROUTES_BEGIN(dns_zone_controller)
    RUVIA_GET("/available", available);
    RUVIA_GET("/", list);
    RUVIA_GET_SSE("/stream", list_stream);
    RUVIA_GET("/options", options);
    RUVIA_GET("/collection", collection);
    RUVIA_POST("/:id/sync", sync);
    RUVIA_GET("/:id", get);
    RUVIA_GET_SSE("/:id/stream", get_stream);
    RUVIA_PUT("/:id", update, dns_zone_config_body);
    RUVIA_POST("/", create, ruvia::json_body<create_dns_zone_body>, create_dns_zone_validator);
    RUVIA_DELETE("/:id", remove);
    RUVIA_ROUTES_END

private:
    struct list_query final {
        std::int64_t page_{};
        std::int64_t page_size_{};
        std::int64_t skip_{};
        std::optional<std::string> keyword_;
        std::optional<std::string> provider_id_;
        std::optional<std::string> sync_status_;
    };

    static list_query parse_list(ruvia::context& c) {
        const auto [page, page_size, skip] = service::common::require_pagination(c);
        return {.page_ = page,
            .page_size_ = page_size,
            .skip_ = skip,
            .keyword_ = service::common::require_keyword(c.req().query("keyword")),
            .provider_id_ = service::common::optional_uuid_query(c, "dns_provider_id"),
            .sync_status_ = service::common::require_enum_query(c, "sync_status",
                {dns_zone_issue_filter, "pending", "synced", "failed"}, "sync_status 不正确")};
    }

    static ruvia::task<dns_zone_page_response> read_list(auto& read, const std::string& tenant,
        const list_query& q) {
        auto data = co_await get_dns_zone_read_service().list(read, tenant, q.page_, q.page_size_, q.skip_,
            q.keyword_, q.provider_id_, q.sync_status_);
        co_return service::common::ok<dns_zone_page_response>(read, std::move(data));
    }

    static ruvia::task<dns_zone_detail_response> read_detail(auto& read, const std::string& tenant,
        const std::string& id) {
        auto data = co_await get_dns_zone_read_service().get(read, tenant, id);
        co_return service::common::ok<dns_zone_detail_response>(read, std::move(data));
    }

    ruvia::task<ruvia::http_response> collection(ruvia::context& c) {
        const auto keyword = service::common::require_keyword(c.req().query("keyword"));
        auto data = co_await get_dns_zone_read_service().options(c,
            service::middleware::current_tenant_id(c), keyword, std::nullopt, std::nullopt, true);
        co_return c.json(service::common::ok<dns_zone_collection_response>(c,
            std::move(data.template ensure<"list">())));
    }

    ruvia::task<ruvia::http_response> available(ruvia::context& c) {
        const auto provider_id = service::common::require_uuid_query(c, "dns_provider_id");
        auto data = co_await get_dns_zone_read_service().available(c,
            service::middleware::current_tenant_id(c), provider_id);
        co_return c.json(service::common::ok<available_dns_zone_list_response>(c, std::move(data)));
    }

    ruvia::task<ruvia::http_response> list(ruvia::context& c) {
        co_return c.json(co_await read_list(c, service::middleware::current_tenant_id(c), parse_list(c)));
    }

    ruvia::task<void> list_stream(ruvia::context& c) {
        const auto q = parse_list(c);
        const auto tenant = service::middleware::current_tenant_id(c);
        auto sub = service::live_resource::get_hub(c).subscribe(c.worker(), tenant,
            service::live_resource::resource::dns_zones, {},
            service::live_resource::query_key("list", q.page_, q.page_size_, q.keyword_.value_or(""),
                q.provider_id_, q.sync_status_));
        co_await service::live_resource::stream_snapshot(
            c, std::move(sub), [tenant, q](auto& read) -> ruvia::task<dns_zone_page_response> {
                co_return co_await read_list(read, tenant, q);
            });
    }

    ruvia::task<ruvia::http_response> options(ruvia::context& c) {
        const auto keyword = service::common::require_keyword(c.req().query("keyword"));
        std::optional<std::string> owner_of;
        if (const auto value = c.req().query("owner_of")) {
            if (!service::common::is_hostname(*value)) {
                service::common::throw_app_error(service::common::validation_error_code,
                    "owner_of 域名不正确", 400);
            }
            owner_of.emplace(*value);
        }
        const auto available = service::common::optional_bool_query(
            c, "available", "available 必须是 true 或 false");
        auto data = co_await get_dns_zone_read_service().options(c,
            service::middleware::current_tenant_id(c), keyword, owner_of, available);
        co_return c.json(service::common::ok<dns_zone_option_list_response>(c, std::move(data)));
    }

    ruvia::task<ruvia::http_response> create(ruvia::context& c) {
        const auto revision = co_await get_dns_zone_command_service().create(c,
            service::middleware::current_tenant_id(c), c.req().validated<create_dns_zone_body>());
        service::common::set_revision_etag(c, revision);
        co_return c.json(service::common::operation(c, "域名已保存，同步任务已提交"));
    }

    ruvia::task<ruvia::http_response> get(ruvia::context& c) {
        co_return c.json(co_await read_detail(c, service::middleware::current_tenant_id(c),
            service::common::require_uuid_param(c, "id")));
    }

    ruvia::task<void> get_stream(ruvia::context& c) {
        const auto id = service::common::require_uuid_param(c, "id");
        const auto tenant = service::middleware::current_tenant_id(c);
        auto sub = service::live_resource::get_hub(c).subscribe(c.worker(), tenant,
            service::live_resource::resource::dns_zones, id,
            service::live_resource::query_key("detail"));
        co_await service::live_resource::stream_snapshot(
            c, std::move(sub), [tenant, id](auto& read) -> ruvia::task<dns_zone_detail_response> {
                co_return co_await read_detail(read, tenant, id);
            });
    }

    ruvia::task<ruvia::http_response> update(ruvia::context& c) {
        const auto expected_revision = service::common::require_expected_revision(c);
        const auto revision = co_await get_dns_zone_command_service().update_config(c,
            service::middleware::current_tenant_id(c), service::common::require_uuid_param(c, "id"),
            expected_revision, c.request_state<service::dns_sync::zone_config_input>());
        service::common::set_revision_etag(c, revision);
        co_return c.json(service::common::operation(c, "DNS 记录已更新"));
    }

    ruvia::task<ruvia::http_response> sync(ruvia::context& c) {
        co_await get_dns_zone_command_service().request_sync(c, service::middleware::current_tenant_id(c),
            service::common::require_uuid_param(c, "id"));
        co_return c.json(service::common::operation(c, "同步任务已提交"));
    }

    ruvia::task<ruvia::http_response> remove(ruvia::context& c) {
        const auto expected_revision = service::common::require_expected_revision(c);
        const auto revision = co_await get_dns_zone_command_service().remove(c,
            service::middleware::current_tenant_id(c), service::common::require_uuid_param(c, "id"),
            expected_revision);
        service::common::set_revision_etag(c, revision);
        co_return c.json(service::common::operation(c, "托管域名已移除"));
    }
};

}  // namespace service::dns_zone
