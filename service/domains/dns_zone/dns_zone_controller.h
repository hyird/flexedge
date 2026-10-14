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
    RUVIA_GET_SSE("/available/stream", available);
    RUVIA_GET_SSE("/stream", list);
    RUVIA_GET_SSE("/options/stream", options);
    RUVIA_GET_SSE("/collection/stream", collection);
    RUVIA_POST("/:id/sync", sync);
    RUVIA_GET_SSE("/:id/stream", get);
    RUVIA_PUT("/:id", update, dns_zone_config_body);
    RUVIA_POST("/", create, ruvia::json_body<create_dns_zone_body>, create_dns_zone_validator);
    RUVIA_DELETE("/:id", remove);
    RUVIA_ROUTES_END

private:
    ruvia::task<void> collection(ruvia::context& c) {
        const auto keyword = service::common::require_keyword(c.req().query("keyword"));
        const auto tenant = tenant_id(c);
        auto sub = service::live_resource::get_hub().subscribe(c.worker(), tenant,
            service::live_resource::resource::dns_zones, {},
            service::live_resource::query_key("collection", keyword.value_or("")));
        co_await service::live_resource::stream_snapshot(
            c, std::move(sub), [tenant, keyword](auto& read) -> ruvia::task<std::string> {
                auto data = co_await get_dns_zone_read_service().options(read, tenant, keyword, std::nullopt, std::nullopt, true);
                co_return std::string(ruvia::to_json(
                    service::common::ok<dns_zone_collection_response>(read, std::move(data.template ensure<"list">())),
                    {.resource_ = read.pool()}));
            });
    }

    static std::string require_id(ruvia::context& c) {
        return service::common::require_uuid_param(c, "id");
    }

    static const std::string& tenant_id(ruvia::context& c) {
        return service::middleware::current_tenant_id(c);
    }

    static std::int64_t expected_revision(ruvia::context& c) {
        return service::common::require_expected_revision(c);
    }

    ruvia::task<void> available(ruvia::context& c) {
        const auto provider_id = service::common::parse_uuid(c.req().query("dns_provider_id"));
        if (!provider_id) {
            service::common::throw_app_error(service::common::validation_error_code,
                "dns_provider_id 必须是 UUID", 400);
        }
        const auto tenant = tenant_id(c);
        auto sub = service::live_resource::get_hub().subscribe(c.worker(), tenant,
            service::live_resource::resource::dns_zones, {},
            service::live_resource::query_key("available", *provider_id));
        co_await service::live_resource::stream_snapshot(
            c, std::move(sub), [tenant, provider_id](auto& read) -> ruvia::task<std::string> {
                auto data = co_await get_dns_zone_read_service().available(read, tenant, *provider_id);
                co_return std::string(ruvia::to_json(
                    service::common::ok<available_dns_zone_list_response>(read, std::move(data)),
                    {.resource_ = read.pool()}));
            });
    }

    ruvia::task<void> list(ruvia::context& c) {
        const auto [page, page_size, skip] = service::common::require_pagination(c);
        const auto keyword = service::common::require_keyword(c.req().query("keyword"));
        std::optional<std::string> provider_id;
        if (const auto value = c.req().query("dns_provider_id")) {
            provider_id = service::common::parse_uuid(value);
            if (!provider_id) {
                service::common::throw_app_error(service::common::validation_error_code,
                    "dns_provider_id 必须是 UUID", 400);
            }
        }
        const auto sync_status = parse_sync_status_filter(c.req().query("sync_status"));
        const auto tenant = tenant_id(c);
        auto sub = service::live_resource::get_hub().subscribe(c.worker(), tenant,
            service::live_resource::resource::dns_zones, {},
            service::live_resource::query_key("list", page, page_size, keyword.value_or(""),
                provider_id, sync_status));
        co_await service::live_resource::stream_snapshot(
            c, std::move(sub), [tenant, page, page_size, skip, keyword, provider_id, sync_status](auto& read) -> ruvia::task<std::string> {
                auto data = co_await get_dns_zone_read_service().list(read, tenant, page, page_size, skip, keyword, provider_id, sync_status);
                co_return std::string(ruvia::to_json(
                    service::common::ok<dns_zone_page_response>(read, std::move(data)),
                    {.resource_ = read.pool()}));
            });
    }

    ruvia::task<void> options(ruvia::context& c) {
        const auto keyword = service::common::require_keyword(c.req().query("keyword"));
        std::optional<std::string> owner_of;
        if (const auto value = c.req().query("owner_of")) {
            if (!service::common::is_hostname(*value)) {
                service::common::throw_app_error(service::common::validation_error_code,
                    "owner_of 域名不正确", 400);
            }
            owner_of.emplace(*value);
        }
        std::optional<bool> available;
        if (const auto value = c.req().query("available")) {
            available = service::common::parse_boolean(value);
            if (!available) {
                service::common::throw_app_error(service::common::validation_error_code,
                    "available 必须是 true 或 false", 400);
            }
        }
        const auto tenant = tenant_id(c);
        auto sub = service::live_resource::get_hub().subscribe(c.worker(), tenant,
            service::live_resource::resource::dns_zones, {},
            service::live_resource::query_key("options", keyword.value_or(""), owner_of,
                available));
        co_await service::live_resource::stream_snapshot(
            c, std::move(sub), [tenant, keyword, owner_of, available](auto& read) -> ruvia::task<std::string> {
                auto data = co_await get_dns_zone_read_service().options(read, tenant, keyword, owner_of, available);
                co_return std::string(ruvia::to_json(
                    service::common::ok<dns_zone_option_list_response>(read, std::move(data)),
                    {.resource_ = read.pool()}));
            });
    }

    ruvia::task<ruvia::http_response> create(ruvia::context& c) {
        co_await get_dns_zone_command_service().create(c, tenant_id(c),
            c.req().validated<create_dns_zone_body>());
        service::common::set_revision_etag(c, 1);
        co_return c.json(service::common::operation(c, "域名已保存，同步任务已提交"));
    }

    ruvia::task<void> get(ruvia::context& c) {
        const auto id = require_id(c);
        const auto tenant = tenant_id(c);
        auto sub = service::live_resource::get_hub().subscribe(c.worker(), tenant,
            service::live_resource::resource::dns_zones, id,
            service::live_resource::query_key("detail"));
        co_await service::live_resource::stream_snapshot(
            c, std::move(sub), [tenant, id](auto& read) -> ruvia::task<std::string> {
                auto data = co_await get_dns_zone_read_service().get(read, tenant, id);
                co_return std::string(ruvia::to_json(
                    service::common::ok<dns_zone_detail_response>(read, std::move(data)),
                    {.resource_ = read.pool()}));
            });
    }

    ruvia::task<ruvia::http_response> update(ruvia::context& c) {
        const auto revision = expected_revision(c);
        co_await get_dns_zone_command_service().update_config(
            c, tenant_id(c), require_id(c), revision,
            c.request_state<service::dns_sync::zone_config_input>());
        service::common::set_revision_etag(c, revision + 1);
        co_return c.json(service::common::operation(c, "配置已保存，同步任务已提交"));
    }

    ruvia::task<ruvia::http_response> sync(ruvia::context& c) {
        std::string_view conflict_policy;
        if (auto body = co_await c.req().json_if<dns_zone_sync_body>()) {
            ruvia::validator validator({.resource_ = c.pool()});
            validate_dns_zone_sync(*body, validator);
            validator.throw_if_invalid();
            if (const auto& value = body->get<"conflict_policy">()) {
                conflict_policy = value->view();
            }
        }
        co_await get_dns_zone_command_service().request_sync(c, tenant_id(c), require_id(c), conflict_policy);
        co_return c.json(service::common::operation(c, "同步任务已提交"));
    }

    ruvia::task<ruvia::http_response> remove(ruvia::context& c) {
        const auto revision = expected_revision(c);
        co_await get_dns_zone_command_service().remove(c, tenant_id(c), require_id(c), revision);
        service::common::set_revision_etag(c, revision + 1);
        co_return c.json(service::common::operation(c, "托管域名已移除"));
    }
};

}  // namespace service::dns_zone
