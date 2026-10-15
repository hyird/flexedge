#pragma once
#include <ruvia/core/task.h>
#include <ruvia/web/context.h>
#include <ruvia/web/controller.h>
#include <ruvia/web/model_json.h>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "service/common/http.h"
#include "service/common/types.h"
#include "service/domains/provider/certificate_provider_schema.h"
#include "service/domains/provider/certificate_provider_service.h"
#include "service/domains/provider/dns_provider_schema.h"
#include "service/domains/provider/dns_provider_service.h"
#include "service/features/live_resource/fanout.h"
#include "service/features/live_resource/sse.h"
#include "service/middleware/auth.h"
namespace service::provider {
class provider_controller final {
public:
    RUVIA_CONTROLLER_GROUP("/api/providers", service::middleware::auth_middleware)
    RUVIA_ROUTES_BEGIN(provider_controller)
    RUVIA_GET_SSE("/dns/:id/stream", get_dns);
    RUVIA_GET_SSE("/dns/stream", list_dns);
    RUVIA_GET_SSE("/dns/options/stream", options_dns);
    RUVIA_POST("/dns", create_dns, ruvia::json_body<create_dns_provider_body>,
        create_dns_provider_validator);
    RUVIA_PUT("/dns/:id", update_dns, ruvia::json_body<update_dns_provider_body>,
        update_dns_provider_validator);
    RUVIA_POST("/dns/:id/verify", verify_dns);
    RUVIA_DELETE("/dns/:id", remove_dns);
    RUVIA_GET_SSE("/certificate/stream", list_certificates);
    RUVIA_GET_SSE("/certificate/options/stream", list_certificates);
    RUVIA_POST("/certificate", create_certificate,
        ruvia::json_body<create_certificate_provider_body>, create_certificate_provider_validator);
    RUVIA_PUT("/certificate/:id", update_certificate,
        ruvia::json_body<update_certificate_provider_body>, update_certificate_provider_validator);
    RUVIA_POST("/certificate/:id/verify", verify_certificate);
    RUVIA_DELETE("/certificate/:id", remove_certificate);
    RUVIA_ROUTES_END
private:
    static const std::string& tenant_id(ruvia::context& c) {
        return service::middleware::current_tenant_id(c);
    }
    static std::string require_id(ruvia::context& c) {
        return service::common::require_uuid_param(c, "id");
    }
    static std::int64_t expected_revision(ruvia::context& c) {
        return service::common::require_expected_revision(c);
    }
    ruvia::task<void> options_dns(ruvia::context& c) {
        const auto keyword = service::common::require_keyword(c.req().query("keyword"));
        std::optional<std::string> status;
        if (const auto value = c.req().query("status")) {
            if (*value != "unverified" && *value != "verified" && *value != "invalid") {
                service::common::throw_app_error(service::common::validation_error_code,
                    "status 不正确", 400);
            }
            status.emplace(*value);
        }
        const auto tenant = tenant_id(c);
        auto sub = service::live_resource::get_hub(c).subscribe(
            c.worker(), tenant, service::live_resource::resource::providers, {},
            service::live_resource::query_key("dns-options", keyword.value_or(""), status));
        co_await service::live_resource::stream_snapshot(
            c, std::move(sub),
            [tenant, keyword, status](auto& read) -> ruvia::task<std::string> {
                ruvia::array<dns_provider_dto> values({.resource_ = read.pool()});
                for (std::int64_t page_number = 1;; ++page_number) {
                    auto page = co_await get_dns_provider_read_service().list(
                        read, tenant, page_number, 1000, (page_number - 1) * 1000, keyword, status);
                    for (auto& item : page.template ensure<"list">()) {
                        values.push_back(std::move(item));
                    }
                    if (page_number >= page.template get<"total_pages">().value_) {
                        break;
                    }
                }
                auto response =
                    service::common::ok<dns_provider_options_response>(read, std::move(values));
                const auto json = ruvia::to_json(response, {.resource_ = read.pool()});
                co_return std::string(json.data(), json.size());
            });
    }
    ruvia::task<void> list_dns(ruvia::context& c) {
        auto [page, size, skip] = service::common::require_pagination(c);
        auto keyword = service::common::require_keyword(c.req().query("keyword"));
        std::optional<std::string> status;
        if (auto v = c.req().query("status")) {
            if (*v != "unverified" && *v != "verified" && *v != "invalid") {
                service::common::throw_app_error(service::common::validation_error_code,
                    "status 不正确", 400);
            }
            status.emplace(*v);
        }
        auto tenant = tenant_id(c);
        auto sub = service::live_resource::get_hub(c).subscribe(
            c.worker(), tenant, service::live_resource::resource::providers, {},
            service::live_resource::query_key("dns-list", page, size, keyword.value_or(""),
                status));
        co_await service::live_resource::stream_snapshot(
            c, std::move(sub),
            [tenant, page, size, skip, keyword,
                status](auto& r) -> ruvia::task<std::string> {
                auto data = co_await get_dns_provider_read_service().list(r, tenant, page, size, skip,
                    keyword, status);
                auto response = service::common::ok<dns_provider_page_response>(r, std::move(data));
                auto json = ruvia::to_json(response, {.resource_ = r.pool()});
                co_return std::string(json.data(), json.size());
            });
    }
    ruvia::task<void> get_dns(ruvia::context& c) {
        auto id = require_id(c);
        auto tenant = tenant_id(c);
        auto sub = service::live_resource::get_hub(c).subscribe(
            c.worker(), tenant, service::live_resource::resource::providers, id,
            service::live_resource::query_key("dns-detail"));
        co_await service::live_resource::stream_snapshot(
            c, std::move(sub),
            [tenant, id](auto& r) -> ruvia::task<std::string> {
                auto data = co_await get_dns_provider_read_service().get(r, tenant, id);
                auto response = service::common::ok<dns_provider_detail_response>(r, std::move(data));
                auto json = ruvia::to_json(response, {.resource_ = r.pool()});
                co_return std::string(json.data(), json.size());
            });
    }
    ruvia::task<void> list_certificates(ruvia::context& c) {
        auto tenant = tenant_id(c);
        auto sub = service::live_resource::get_hub(c).subscribe(
            c.worker(), tenant, service::live_resource::resource::providers, {},
            service::live_resource::query_key("certificate-list"));
        co_await service::live_resource::stream_snapshot(
            c, std::move(sub), [tenant](auto& r) -> ruvia::task<std::string> {
                auto data = co_await get_certificate_provider_read_service().list(r, tenant);
                auto response =
                    service::common::ok<certificate_provider_list_response>(r, std::move(data));
                auto json = ruvia::to_json(response, {.resource_ = r.pool()});
                co_return std::string(json.data(), json.size());
            });
    }
    ruvia::task<ruvia::http_response> create_dns(ruvia::context& c) {
        co_await get_dns_provider_service().create(c, tenant_id(c),
            c.req().validated<create_dns_provider_body>());
        service::common::set_revision_etag(c, 1);
        co_return c.json(service::common::operation(c, "DNS 服务商账号已添加"));
    }
    ruvia::task<ruvia::http_response> update_dns(ruvia::context& c) {
        auto v = expected_revision(c);
        const auto operation_id = co_await get_dns_provider_service().update(c, tenant_id(c), require_id(c), v,
            c.req().validated<update_dns_provider_body>());
        service::common::set_revision_etag(c, v + 1);
        co_return c.json(service::common::operation(c, "DNS 服务商账号已更新", operation_id));
    }
    ruvia::task<ruvia::http_response> verify_dns(ruvia::context& c) {
        auto v = expected_revision(c);
        const auto operation_id =
            co_await get_dns_provider_service().verify_stored(c, tenant_id(c), require_id(c), v);
        service::common::set_revision_etag(c, v);
        co_return c.json(service::common::operation(c, "DNS 服务商凭据检测任务已提交", operation_id));
    }
    ruvia::task<ruvia::http_response> remove_dns(ruvia::context& c) {
        auto v = expected_revision(c);
        const auto operation_id = co_await get_dns_provider_service().remove(c, tenant_id(c), require_id(c), v);
        service::common::set_revision_etag(c, v + 1);
        co_return c.json(service::common::operation(c, "DNS 服务商账号已移除", operation_id));
    }
    ruvia::task<ruvia::http_response> create_certificate(ruvia::context& c) {
        co_await get_certificate_provider_service().create(
            c, tenant_id(c), c.req().validated<create_certificate_provider_body>());
        service::common::set_revision_etag(c, 1);
        co_return c.json(service::common::operation(c, "证书供应商已保存"));
    }
    ruvia::task<ruvia::http_response> update_certificate(ruvia::context& c) {
        auto v = expected_revision(c);
        const auto operation_id = co_await get_certificate_provider_service().update(
            c, tenant_id(c), require_id(c), v, c.req().validated<update_certificate_provider_body>());
        service::common::set_revision_etag(c, v + 1);
        co_return c.json(service::common::operation(c, "证书供应商已更新", operation_id));
    }
    ruvia::task<ruvia::http_response> verify_certificate(ruvia::context& c) {
        auto v = expected_revision(c);
        const auto operation_id =
            co_await get_certificate_provider_service().verify(c, tenant_id(c), require_id(c), v);
        service::common::set_revision_etag(c, v);
        co_return c.json(service::common::operation(c, "证书供应商检测任务已提交", operation_id));
    }
    ruvia::task<ruvia::http_response> remove_certificate(ruvia::context& c) {
        auto v = expected_revision(c);
        const auto operation_id =
            co_await get_certificate_provider_service().remove(c, tenant_id(c), require_id(c), v);
        service::common::set_revision_etag(c, v + 1);
        co_return c.json(service::common::operation(c, "证书供应商已删除", operation_id));
    }
};
}  // namespace service::provider
