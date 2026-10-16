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

#include "service/common/http.h"
#include "service/common/types.h"
#include "service/domains/certificate/certificate_command_service.h"
#include "service/domains/certificate/certificate_read_service.h"
#include "service/domains/certificate/certificate_schema.h"
#include "service/features/live_resource/fanout.h"
#include "service/features/live_resource/sse.h"
#include "service/middleware/auth.h"
namespace service::certificate {
class certificate_controller final {
public:
    RUVIA_CONTROLLER_GROUP("/api/certificates", service::middleware::auth_middleware)
    RUVIA_ROUTES_BEGIN(certificate_controller)
    RUVIA_GET_SSE("/stream", list);
    RUVIA_GET_SSE("/options/stream", options);
    RUVIA_POST("/", create, ruvia::json_body<create_certificate_body>, create_certificate_validator);
    RUVIA_POST("/:id/renew", renew);
    RUVIA_GET("/:id/download", download);
    RUVIA_GET_SSE("/:id/stream", get);
    RUVIA_PUT("/:id", update, ruvia::json_body<service::certificate_issuance::certificate_config_input>, certificate_config_validator);
    RUVIA_DELETE("/:id", remove);
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
    static std::optional<std::string> require_status(ruvia::context& c) {
        auto v = c.req().query("status");
        if (v && *v != "pending" && *v != "issuing" && *v != "valid" && *v != "renewing" &&
            *v != "failed" && *v != "expired") {
            service::common::throw_app_error(service::common::validation_error_code, "证书状态不正确",
                400);
        }
        return v ? std::optional<std::string>{std::string(*v)} : std::nullopt;
    }
    static std::optional<bool> require_usable(ruvia::context& c) {
        auto v = c.req().query("usable");
        if (!v) {
            return std::nullopt;
        }
        auto r = service::common::parse_boolean(v);
        if (!r) {
            service::common::throw_app_error(service::common::validation_error_code,
                "usable 必须是 true 或 false", 400);
        }
        return r;
    }
    ruvia::task<void> options(ruvia::context& c) {
        auto keyword = service::common::require_keyword(c.req().query("keyword"));
        auto status = require_status(c);
        auto usable = require_usable(c);
        auto tenant = tenant_id(c);
        auto sub = service::live_resource::get_hub(c).subscribe(
            c.worker(), tenant, service::live_resource::resource::certificates, {},
            service::live_resource::query_key("options", keyword.value_or(""), status, usable));
        co_await service::live_resource::stream_snapshot(
            c, std::move(sub),
            [tenant, keyword, status,
                usable](auto& r) -> ruvia::task<std::string> {
                ruvia::array<certificate_dto> data({.resource_ = r.pool()});
                for (std::int64_t page_number = 1;; ++page_number) {
                    auto page = co_await get_certificate_read_service().list(r, tenant, page_number, 1000,
                        (page_number - 1) * 1000,
                        keyword, status, usable,
                        false, false);
                    for (auto& item : page.template ensure<"list">()) {
                        data.push_back(std::move(item));
                    }
                    if (page_number >= page.template get<"total_pages">().value_) {
                        break;
                    }
                }
                auto response = service::common::ok<certificate_options_response>(r, std::move(data));
                auto json = ruvia::to_json(response, {.resource_ = r.pool()});
                co_return std::string(json.data(), json.size());
            });
    }
    ruvia::task<void> list(ruvia::context& c) {
        auto [page, size, skip] = service::common::require_pagination(c);
        auto keyword = service::common::require_keyword(c.req().query("keyword"));
        auto status = require_status(c);
        auto usable = require_usable(c);
        const auto expiring = parse_attention_filter(c.req().query("expiring"), "expiring");
        const auto abnormal = parse_attention_filter(c.req().query("abnormal"), "abnormal");
        auto tenant = tenant_id(c);
        auto sub = service::live_resource::get_hub(c).subscribe(
            c.worker(), tenant, service::live_resource::resource::certificates, {},
            service::live_resource::query_key("list", page, size, keyword.value_or(""), status,
                usable, expiring, abnormal));
        co_await service::live_resource::stream_snapshot(
            c, std::move(sub),
            [tenant, page, size, skip, keyword, status, usable, expiring,
                abnormal](auto& r) -> ruvia::task<std::string> {
                auto data = co_await get_certificate_read_service().list(r, tenant, page, size, skip,
                    keyword, status, usable,
                    expiring, abnormal);
                auto response = service::common::ok<certificate_page_response>(r, std::move(data));
                auto json = ruvia::to_json(response, {.resource_ = r.pool()});
                co_return std::string(json.data(), json.size());
            });
    }
    ruvia::task<void> get(ruvia::context& c) {
        auto id = require_id(c);
        auto tenant = tenant_id(c);
        auto sub = service::live_resource::get_hub(c).subscribe(
            c.worker(), tenant, service::live_resource::resource::certificates, id,
            service::live_resource::query_key("detail"));
        co_await service::live_resource::stream_snapshot(
            c, std::move(sub),
            [tenant, id](auto& r) -> ruvia::task<std::string> {
                auto data = co_await get_certificate_read_service().get(r, tenant, id);
                auto response = service::common::ok<certificate_detail_response>(r, std::move(data));
                auto json = ruvia::to_json(response, {.resource_ = r.pool()});
                co_return std::string(json.data(), json.size());
            });
    }
    ruvia::task<ruvia::http_response> create(ruvia::context& c) {
        const auto operation_id = co_await get_certificate_command_service().create(c, tenant_id(c),
            c.req().validated<create_certificate_body>());
        service::common::set_revision_etag(c, 1);
        co_return c.json(service::common::operation(c, "证书申请已提交", operation_id));
    }
    ruvia::task<ruvia::http_response> update(ruvia::context& c) {
        auto v = expected_revision(c);
        const auto operation_id = co_await get_certificate_command_service().update(
            c, tenant_id(c), require_id(c), v,
            c.req().validated_json<service::certificate_issuance::certificate_config_input>());
        service::common::set_revision_etag(c, v + 1);
        co_return c.json(service::common::operation(c, "证书设置已更新", operation_id));
    }
    ruvia::task<ruvia::http_response> renew(ruvia::context& c) {
        auto v = expected_revision(c);
        const auto operation_id = co_await get_certificate_command_service().renew(c, tenant_id(c), require_id(c), v);
        service::common::set_revision_etag(c, v);
        co_return c.json(service::common::operation(c, "重新签发已提交", operation_id));
    }
    ruvia::task<ruvia::http_response> download(ruvia::context& c) {
        auto d = co_await get_certificate_read_service().download(c, tenant_id(c), require_id(c));
        c.header("cache-control", "no-store");
        c.header("content-disposition", "attachment; filename=\"" + d.filename_ + "\"");
        c.header("content-type", "application/zip");
        c.header("x-content-type-options", "nosniff");
        co_return c.body(std::string_view(d.archive_));
    }
    ruvia::task<ruvia::http_response> remove(ruvia::context& c) {
        auto v = expected_revision(c);
        const auto operation_id =
            co_await get_certificate_command_service().remove(c, tenant_id(c), require_id(c), v);
        service::common::set_revision_etag(c, v + 1);
        co_return c.json(service::common::operation(c, "证书已删除", operation_id));
    }
};
}  // namespace service::certificate
