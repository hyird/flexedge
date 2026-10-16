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
#include "service/domains/provider/certificate_provider_read_service.h"
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
    RUVIA_GET("/dns/options", options_dns);
    RUVIA_GET("/dns", list_dns);
    RUVIA_GET_SSE("/dns/stream", list_dns_stream);
    RUVIA_GET("/dns/:id", get_dns);
    RUVIA_GET_SSE("/dns/:id/stream", get_dns_stream);
    RUVIA_POST("/dns", create_dns, ruvia::json_body<create_dns_provider_body>,
        create_dns_provider_validator);
    RUVIA_PUT("/dns/:id", update_dns, ruvia::json_body<update_dns_provider_body>,
        update_dns_provider_validator);
    RUVIA_POST("/dns/:id/verify", verify_dns);
    RUVIA_DELETE("/dns/:id", remove_dns);
    RUVIA_GET("/certificate", list_certificates);
    RUVIA_GET("/certificate/options", options_certificates);
    RUVIA_GET_SSE("/certificate/stream", list_certificates_stream);
    RUVIA_POST("/certificate", create_certificate,
        ruvia::json_body<create_certificate_provider_body>, create_certificate_provider_validator);
    RUVIA_PUT("/certificate/:id", update_certificate,
        ruvia::json_body<update_certificate_provider_body>, update_certificate_provider_validator);
    RUVIA_POST("/certificate/:id/verify", verify_certificate);
    RUVIA_DELETE("/certificate/:id", remove_certificate);
    RUVIA_ROUTES_END
private:
    static std::optional<std::string> require_dns_status(ruvia::context& c) {
        return service::common::require_enum_query(c, "status", {"unverified", "verified", "invalid"},
            "status 不正确");
    }

    struct dns_list_query final {
        std::int64_t page_{};
        std::int64_t size_{};
        std::int64_t skip_{};
        std::optional<std::string> keyword_;
        std::optional<std::string> status_;
    };

    static dns_list_query parse_dns_list(ruvia::context& c) {
        auto [page, size, skip] = service::common::require_pagination(c);
        return {.page_ = page,
            .size_ = size,
            .skip_ = skip,
            .keyword_ = service::common::require_keyword(c.req().query("keyword")),
            .status_ = require_dns_status(c)};
    }

    static ruvia::task<dns_provider_page_response> read_dns_list(auto& r, const std::string& tenant,
        const dns_list_query& q) {
        auto data = co_await get_dns_provider_read_service().list(r, tenant, q.page_, q.size_, q.skip_,
            q.keyword_, q.status_);
        co_return service::common::ok<dns_provider_page_response>(r, std::move(data));
    }

    static ruvia::task<dns_provider_detail_response> read_dns_detail(auto& r, const std::string& tenant,
        const std::string& id) {
        auto data = co_await get_dns_provider_read_service().get(r, tenant, id);
        co_return service::common::ok<dns_provider_detail_response>(r, std::move(data));
    }

    struct certificate_list_query final {
        std::int64_t page_{};
        std::int64_t size_{};
        std::int64_t skip_{};
        std::optional<std::string> keyword_;
    };

    static certificate_list_query parse_certificate_list(ruvia::context& c) {
        auto [page, size, skip] = service::common::require_pagination(c);
        return {.page_ = page,
            .size_ = size,
            .skip_ = skip,
            .keyword_ = service::common::require_keyword(c.req().query("keyword"))};
    }

    static ruvia::task<certificate_provider_page_response> read_certificate_list(auto& r,
        const std::string& tenant, const certificate_list_query& q) {
        auto data = co_await get_certificate_provider_read_service().list(r, tenant, q.page_, q.size_,
            q.skip_, q.keyword_);
        co_return service::common::ok<certificate_provider_page_response>(r, std::move(data));
    }

    ruvia::task<ruvia::http_response> options_dns(ruvia::context& c) {
        const auto keyword = service::common::require_keyword(c.req().query("keyword"));
        const auto status = require_dns_status(c);
        const auto& tenant = service::middleware::current_tenant_id(c);
        ruvia::array<dns_provider_dto> values({.resource_ = c.pool()});
        for (std::int64_t page_number = 1;; ++page_number) {
            auto page = co_await get_dns_provider_read_service().list(
                c, tenant, page_number, 1000, (page_number - 1) * 1000, keyword, status);
            for (auto& item : page.template ensure<"list">()) {
                values.push_back(std::move(item));
            }
            if (page_number >= page.template get<"total_pages">().value_) {
                break;
            }
        }
        co_return c.json(service::common::ok<dns_provider_options_response>(c, std::move(values)));
    }
    ruvia::task<ruvia::http_response> list_dns(ruvia::context& c) {
        co_return c.json(
            co_await read_dns_list(c, service::middleware::current_tenant_id(c), parse_dns_list(c)));
    }
    ruvia::task<void> list_dns_stream(ruvia::context& c) {
        auto q = parse_dns_list(c);
        auto tenant = service::middleware::current_tenant_id(c);
        auto sub = service::live_resource::get_hub(c).subscribe(
            c.worker(), tenant, service::live_resource::resource::providers, {},
            service::live_resource::query_key("dns-list", q.page_, q.size_, q.keyword_.value_or(""),
                q.status_));
        co_await service::live_resource::stream_snapshot(
            c, std::move(sub), [tenant, q](auto& r) -> ruvia::task<std::string> {
                auto response = co_await read_dns_list(r, tenant, q);
                auto json = ruvia::to_json(response, {.resource_ = r.pool()});
                co_return std::string(json.data(), json.size());
            });
    }
    ruvia::task<ruvia::http_response> get_dns(ruvia::context& c) {
        co_return c.json(co_await read_dns_detail(c, service::middleware::current_tenant_id(c),
            service::common::require_uuid_param(c, "id")));
    }
    ruvia::task<void> get_dns_stream(ruvia::context& c) {
        auto id = service::common::require_uuid_param(c, "id");
        auto tenant = service::middleware::current_tenant_id(c);
        auto sub = service::live_resource::get_hub(c).subscribe(
            c.worker(), tenant, service::live_resource::resource::providers, id,
            service::live_resource::query_key("dns-detail"));
        co_await service::live_resource::stream_snapshot(
            c, std::move(sub), [tenant, id](auto& r) -> ruvia::task<std::string> {
                auto response = co_await read_dns_detail(r, tenant, id);
                auto json = ruvia::to_json(response, {.resource_ = r.pool()});
                co_return std::string(json.data(), json.size());
            });
    }
    ruvia::task<ruvia::http_response> options_certificates(ruvia::context& c) {
        const auto keyword = service::common::require_keyword(c.req().query("keyword"));
        const auto& tenant = service::middleware::current_tenant_id(c);
        ruvia::array<certificate_provider_dto> values({.resource_ = c.pool()});
        for (std::int64_t page_number = 1;; ++page_number) {
            auto page = co_await get_certificate_provider_read_service().list(
                c, tenant, page_number, 1000, (page_number - 1) * 1000, keyword);
            for (auto& item : page.template ensure<"list">()) {
                values.push_back(std::move(item));
            }
            if (page_number >= page.template get<"total_pages">().value_) {
                break;
            }
        }
        co_return c.json(
            service::common::ok<certificate_provider_options_response>(c, std::move(values)));
    }
    ruvia::task<ruvia::http_response> list_certificates(ruvia::context& c) {
        co_return c.json(co_await read_certificate_list(c, service::middleware::current_tenant_id(c),
            parse_certificate_list(c)));
    }
    ruvia::task<void> list_certificates_stream(ruvia::context& c) {
        auto q = parse_certificate_list(c);
        auto tenant = service::middleware::current_tenant_id(c);
        auto sub = service::live_resource::get_hub(c).subscribe(
            c.worker(), tenant, service::live_resource::resource::providers, {},
            service::live_resource::query_key("certificate-list", q.page_, q.size_,
                q.keyword_.value_or("")));
        co_await service::live_resource::stream_snapshot(
            c, std::move(sub), [tenant, q](auto& r) -> ruvia::task<std::string> {
                auto response = co_await read_certificate_list(r, tenant, q);
                auto json = ruvia::to_json(response, {.resource_ = r.pool()});
                co_return std::string(json.data(), json.size());
            });
    }
    ruvia::task<ruvia::http_response> create_dns(ruvia::context& c) {
        const auto revision = co_await get_dns_provider_service().create(c,
            service::middleware::current_tenant_id(c), c.req().validated<create_dns_provider_body>());
        service::common::set_revision_etag(c, revision);
        co_return c.json(service::common::operation(c, "DNS 服务商账号已添加"));
    }
    ruvia::task<ruvia::http_response> update_dns(ruvia::context& c) {
        const auto expected_revision = service::common::require_expected_revision(c);
        const auto revision = co_await get_dns_provider_service().update(c,
            service::middleware::current_tenant_id(c), service::common::require_uuid_param(c, "id"),
            expected_revision, c.req().validated<update_dns_provider_body>());
        service::common::set_revision_etag(c, revision);
        co_return c.json(service::common::operation(c, "DNS 服务商账号已更新"));
    }
    ruvia::task<ruvia::http_response> verify_dns(ruvia::context& c) {
        const auto expected_revision = service::common::require_expected_revision(c);
        const auto revision = co_await get_dns_provider_service().verify_stored(c,
            service::middleware::current_tenant_id(c), service::common::require_uuid_param(c, "id"),
            expected_revision);
        service::common::set_revision_etag(c, revision);
        co_return c.json(service::common::operation(c, "DNS 服务商凭据检测任务已提交"));
    }
    ruvia::task<ruvia::http_response> remove_dns(ruvia::context& c) {
        const auto expected_revision = service::common::require_expected_revision(c);
        const auto revision = co_await get_dns_provider_service().remove(c,
            service::middleware::current_tenant_id(c), service::common::require_uuid_param(c, "id"),
            expected_revision);
        service::common::set_revision_etag(c, revision);
        co_return c.json(service::common::operation(c, "DNS 服务商账号已移除"));
    }
    ruvia::task<ruvia::http_response> create_certificate(ruvia::context& c) {
        const auto revision = co_await get_certificate_provider_service().create(
            c, service::middleware::current_tenant_id(c), c.req().validated<create_certificate_provider_body>());
        service::common::set_revision_etag(c, revision);
        co_return c.json(service::common::operation(c, "证书供应商已保存"));
    }
    ruvia::task<ruvia::http_response> update_certificate(ruvia::context& c) {
        const auto expected_revision = service::common::require_expected_revision(c);
        const auto revision = co_await get_certificate_provider_service().update(c,
            service::middleware::current_tenant_id(c), service::common::require_uuid_param(c, "id"),
            expected_revision, c.req().validated<update_certificate_provider_body>());
        service::common::set_revision_etag(c, revision);
        co_return c.json(service::common::operation(c, "证书供应商已更新"));
    }
    ruvia::task<ruvia::http_response> verify_certificate(ruvia::context& c) {
        const auto expected_revision = service::common::require_expected_revision(c);
        const auto revision = co_await get_certificate_provider_service().verify(c,
            service::middleware::current_tenant_id(c), service::common::require_uuid_param(c, "id"),
            expected_revision);
        service::common::set_revision_etag(c, revision);
        co_return c.json(service::common::operation(c, "证书供应商检测任务已提交"));
    }
    ruvia::task<ruvia::http_response> remove_certificate(ruvia::context& c) {
        const auto expected_revision = service::common::require_expected_revision(c);
        const auto revision = co_await get_certificate_provider_service().remove(c,
            service::middleware::current_tenant_id(c), service::common::require_uuid_param(c, "id"),
            expected_revision);
        service::common::set_revision_etag(c, revision);
        co_return c.json(service::common::operation(c, "证书供应商已删除"));
    }
};
}  // namespace service::provider
