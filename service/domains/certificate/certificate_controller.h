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
    RUVIA_GET("/", list);
    RUVIA_GET_SSE("/stream", list_stream);
    RUVIA_GET("/options", options);
    RUVIA_POST("/", create, ruvia::json_body<create_certificate_body>, create_certificate_validator);
    RUVIA_POST("/:id/renew", renew);
    RUVIA_GET("/:id/download", download);
    RUVIA_GET("/:id", get);
    RUVIA_GET_SSE("/:id/stream", get_stream);
    RUVIA_PUT("/:id", update, ruvia::json_body<service::certificate_issuance::certificate_config_input>, certificate_config_validator);
    RUVIA_DELETE("/:id", remove);
    RUVIA_ROUTES_END
private:
    static std::optional<std::string> require_status(ruvia::context& c) {
        return service::common::require_enum_query(c, "status",
            {"pending", "issuing", "valid", "renewing", "failed", "expired"}, "证书状态不正确");
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

    struct list_query final {
        std::int64_t page_{};
        std::int64_t size_{};
        std::int64_t skip_{};
        std::optional<std::string> keyword_;
        std::optional<std::string> status_;
        std::optional<bool> usable_;
        bool expiring_{};
        bool abnormal_{};
    };

    static list_query parse_list(ruvia::context& c) {
        auto [page, size, skip] = service::common::require_pagination(c);
        return {.page_ = page,
            .size_ = size,
            .skip_ = skip,
            .keyword_ = service::common::require_keyword(c.req().query("keyword")),
            .status_ = require_status(c),
            .usable_ = require_usable(c),
            .expiring_ = parse_attention_filter(c.req().query("expiring"), "expiring"),
            .abnormal_ = parse_attention_filter(c.req().query("abnormal"), "abnormal")};
    }

    static ruvia::task<certificate_page_response> read_list(auto& r, const std::string& tenant,
        const list_query& q) {
        auto data = co_await get_certificate_read_service().list(r, tenant, q.page_, q.size_, q.skip_,
            q.keyword_, q.status_, q.usable_, q.expiring_, q.abnormal_);
        co_return service::common::ok<certificate_page_response>(r, std::move(data));
    }

    static ruvia::task<certificate_detail_response> read_detail(auto& r, const std::string& tenant,
        const std::string& id) {
        auto data = co_await get_certificate_read_service().get(r, tenant, id);
        co_return service::common::ok<certificate_detail_response>(r, std::move(data));
    }

    ruvia::task<ruvia::http_response> options(ruvia::context& c) {
        auto keyword = service::common::require_keyword(c.req().query("keyword"));
        auto status = require_status(c);
        auto usable = require_usable(c);
        const auto& tenant = service::middleware::current_tenant_id(c);
        auto data = co_await get_certificate_read_service().options(c, tenant, keyword, status, usable);
        co_return c.json(service::common::ok<certificate_options_response>(c, std::move(data)));
    }
    ruvia::task<ruvia::http_response> list(ruvia::context& c) {
        co_return c.json(co_await read_list(c, service::middleware::current_tenant_id(c), parse_list(c)));
    }
    ruvia::task<void> list_stream(ruvia::context& c) {
        auto q = parse_list(c);
        auto tenant = service::middleware::current_tenant_id(c);
        auto sub = service::live_resource::get_hub(c).subscribe(
            c.worker(), tenant, service::live_resource::resource::certificates, {},
            service::live_resource::query_key("list", q.page_, q.size_, q.keyword_.value_or(""),
                q.status_, q.usable_, q.expiring_, q.abnormal_));
        co_await service::live_resource::stream_snapshot(
            c, std::move(sub), [tenant, q](auto& r) -> ruvia::task<std::string> {
                auto response = co_await read_list(r, tenant, q);
                auto json = ruvia::to_json(response, {.resource_ = r.pool()});
                co_return std::string(json.data(), json.size());
            });
    }
    ruvia::task<ruvia::http_response> get(ruvia::context& c) {
        co_return c.json(co_await read_detail(c, service::middleware::current_tenant_id(c),
            service::common::require_uuid_param(c, "id")));
    }
    ruvia::task<void> get_stream(ruvia::context& c) {
        auto id = service::common::require_uuid_param(c, "id");
        auto tenant = service::middleware::current_tenant_id(c);
        auto sub = service::live_resource::get_hub(c).subscribe(
            c.worker(), tenant, service::live_resource::resource::certificates, id,
            service::live_resource::query_key("detail"));
        co_await service::live_resource::stream_snapshot(
            c, std::move(sub), [tenant, id](auto& r) -> ruvia::task<std::string> {
                auto response = co_await read_detail(r, tenant, id);
                auto json = ruvia::to_json(response, {.resource_ = r.pool()});
                co_return std::string(json.data(), json.size());
            });
    }
    ruvia::task<ruvia::http_response> create(ruvia::context& c) {
        const auto revision = co_await get_certificate_command_service().create(c,
            service::middleware::current_tenant_id(c), c.req().validated<create_certificate_body>());
        service::common::set_revision_etag(c, revision);
        co_return c.json(service::common::operation(c, "证书申请已提交"));
    }
    ruvia::task<ruvia::http_response> update(ruvia::context& c) {
        const auto expected_revision = service::common::require_expected_revision(c);
        const auto revision = co_await get_certificate_command_service().update(c,
            service::middleware::current_tenant_id(c), service::common::require_uuid_param(c, "id"),
            expected_revision,
            c.req().validated_json<service::certificate_issuance::certificate_config_input>());
        service::common::set_revision_etag(c, revision);
        co_return c.json(service::common::operation(c, "证书设置已更新"));
    }
    ruvia::task<ruvia::http_response> renew(ruvia::context& c) {
        const auto expected_revision = service::common::require_expected_revision(c);
        const auto revision = co_await get_certificate_command_service().renew(c,
            service::middleware::current_tenant_id(c), service::common::require_uuid_param(c, "id"),
            expected_revision);
        service::common::set_revision_etag(c, revision);
        co_return c.json(service::common::operation(c, "重新签发已提交"));
    }
    ruvia::task<ruvia::http_response> download(ruvia::context& c) {
        auto d = co_await get_certificate_read_service().download(c, service::middleware::current_tenant_id(c),
            service::common::require_uuid_param(c, "id"));
        c.header("cache-control", "no-store");
        c.header("content-disposition", "attachment; filename=\"" + d.filename_ + "\"");
        c.header("content-type", "application/zip");
        c.header("x-content-type-options", "nosniff");
        co_return c.body(std::string_view(d.archive_));
    }
    ruvia::task<ruvia::http_response> remove(ruvia::context& c) {
        const auto expected_revision = service::common::require_expected_revision(c);
        const auto revision = co_await get_certificate_command_service().remove(c,
            service::middleware::current_tenant_id(c), service::common::require_uuid_param(c, "id"),
            expected_revision);
        service::common::set_revision_etag(c, revision);
        co_return c.json(service::common::operation(c, "证书已删除"));
    }
};
}  // namespace service::certificate
