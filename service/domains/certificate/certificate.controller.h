#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <ruvia/core/Task.h>
#include <ruvia/web/Context.h>
#include <ruvia/web/ModelJson.h>
#include <ruvia/web/Controller.h>
#include "service/common/http.h"
#include "service/common/types.h"
#include "service/domains/certificate/certificate.schema.h"
#include "service/domains/certificate/certificate_command.service.h"
#include "service/domains/certificate/certificate_read.service.h"
#include "service/middleware/auth.h"
#include "service/features/live_resource/fanout.h"
#include "service/features/live_resource/sse.h"
namespace service::certificate {
class CertificateController final : public ruvia::Controller<CertificateController> {
  public:
    RUVIA_CONTROLLER_GROUP("/api/certificates", service::middleware::AuthMiddleware)
    RUVIA_ROUTES_BEGIN
    RUVIA_GET_SSE("/stream", list);
    RUVIA_GET_SSE("/options/stream", options);
    RUVIA_POST("/", create, CreateCertificateValidator);
    RUVIA_POST("/:id/renew", renew);
    RUVIA_GET("/:id/download", download);
    RUVIA_GET_SSE("/:id/stream", get);
    RUVIA_PUT("/:id", update, CertificateConfigValidator);
    RUVIA_DELETE("/:id", remove);
    RUVIA_ROUTES_END
  private:
    static const std::string& tenantId(ruvia::Context& c) {
        return service::middleware::currentTenantId(c);
    }
    static std::string requireId(ruvia::Context& c) {
        return service::common::requireUuidParam(c, "id");
    }
    static std::int64_t expectedRevision(ruvia::Context& c) {
        return service::common::requireExpectedRevision(c);
    }
    static std::optional<std::string> requireStatus(ruvia::Context& c) {
        auto v = c.req().query("status");
        if (v && *v != "pending" && *v != "issuing" && *v != "valid" && *v != "renewing" &&
            *v != "failed" && *v != "expired")
            service::common::throwAppError(service::common::kValidationErrorCode, "证书状态不正确",
                                           400);
        return v ? std::optional<std::string>{std::string(*v)} : std::nullopt;
    }
    static std::optional<bool> requireUsable(ruvia::Context& c) {
        auto v = c.req().query("usable");
        if (!v)
            return std::nullopt;
        auto r = service::common::parseBoolean(v);
        if (!r)
            service::common::throwAppError(service::common::kValidationErrorCode,
                                           "usable 必须是 true 或 false", 400);
        return r;
    }
    ruvia::Task<void> options(ruvia::Context& c) {
        auto keyword = service::common::requireKeyword(c.req().query("keyword"));
        auto status = requireStatus(c);
        auto usable = requireUsable(c);
        auto tenant = tenantId(c);
        auto sub = service::live_resource::hub().subscribe(
            c.worker(), tenant, service::live_resource::Resource::certificates, {},
            service::live_resource::queryKey("options", keyword.value_or(""), status, usable));
        co_await service::live_resource::streamSnapshot(
            c, std::move(sub),
            [tenant, keyword, status,
             usable](ruvia::WebWorkerContext& r) -> ruvia::Task<std::string> {
                ruvia::Array<CertificateDto> data(r.resource());
                for (std::int64_t pageNumber = 1;; ++pageNumber) {
                    auto page = co_await certificateReadService().list(r, tenant, pageNumber, 1000,
                                                                       (pageNumber - 1) * 1000,
                                                                       keyword, status, usable);
                    for (auto& item : page.ensure<"list">())
                        data.push_back(std::move(item));
                    if (pageNumber >= page.get<"totalPages">().value)
                        break;
                }
                auto response = service::common::ok<CertificateOptionsResponse>(r, std::move(data));
                auto json = ruvia::toJson(response, {.resource = r.resource()});
                co_return std::string(json.data(), json.size());
            });
    }
    ruvia::Task<void> list(ruvia::Context& c) {
        auto [page, size, skip] = service::common::requirePagination(c);
        auto keyword = service::common::requireKeyword(c.req().query("keyword"));
        auto status = requireStatus(c);
        auto usable = requireUsable(c);
        auto tenant = tenantId(c);
        auto sub = service::live_resource::hub().subscribe(
            c.worker(), tenant, service::live_resource::Resource::certificates, {},
            service::live_resource::queryKey("list", page, size, keyword.value_or(""), status,
                                             usable));
        co_await service::live_resource::streamSnapshot(
            c, std::move(sub),
            [tenant, page, size, skip, keyword, status,
             usable](ruvia::WebWorkerContext& r) -> ruvia::Task<std::string> {
                auto data = co_await certificateReadService().list(r, tenant, page, size, skip,
                                                                   keyword, status, usable);
                auto response = service::common::ok<CertificatePageResponse>(r, std::move(data));
                auto json = ruvia::toJson(response, {.resource = r.resource()});
                co_return std::string(json.data(), json.size());
            });
    }
    ruvia::Task<void> get(ruvia::Context& c) {
        auto id = requireId(c);
        auto tenant = tenantId(c);
        auto sub = service::live_resource::hub().subscribe(
            c.worker(), tenant, service::live_resource::Resource::certificates, id,
            service::live_resource::queryKey("detail"));
        co_await service::live_resource::streamSnapshot(
            c, std::move(sub),
            [tenant, id](ruvia::WebWorkerContext& r) -> ruvia::Task<std::string> {
                auto data = co_await certificateReadService().get(r, tenant, id);
                auto response = service::common::ok<CertificateDetailResponse>(r, std::move(data));
                auto json = ruvia::toJson(response, {.resource = r.resource()});
                co_return std::string(json.data(), json.size());
            });
    }
    ruvia::Task<ruvia::HttpResponse> create(ruvia::Context& c) {
        co_await certificateCommandService().create(c, tenantId(c),
                                                    c.req().validated<CreateCertificateBody>());
        service::common::setRevisionEtag(c, 1);
        co_return c.json(service::common::operation(c, "证书申请已提交"));
    }
    ruvia::Task<ruvia::HttpResponse> update(ruvia::Context& c) {
        auto v = expectedRevision(c);
        co_await certificateCommandService().update(
            c, tenantId(c), requireId(c), v,
            c.req().validatedJson<service::certificate_issuance::CertificateConfigInput>());
        service::common::setRevisionEtag(c, v + 1);
        co_return c.json(service::common::operation(c, "证书设置已更新"));
    }
    ruvia::Task<ruvia::HttpResponse> renew(ruvia::Context& c) {
        auto v = expectedRevision(c);
        co_await certificateCommandService().renew(c, tenantId(c), requireId(c), v);
        service::common::setRevisionEtag(c, v);
        co_return c.json(service::common::operation(c, "重新签发已提交"));
    }
    ruvia::Task<ruvia::HttpResponse> download(ruvia::Context& c) {
        auto d = co_await certificateReadService().download(c, tenantId(c), requireId(c));
        c.header("cache-control", "no-store");
        c.header("content-disposition", "attachment; filename=\"" + d.filename + "\"");
        c.header("content-type", "application/zip");
        c.header("x-content-type-options", "nosniff");
        co_return c.body(std::string_view(d.archive));
    }
    ruvia::Task<ruvia::HttpResponse> remove(ruvia::Context& c) {
        auto v = expectedRevision(c);
        co_await certificateCommandService().remove(c, tenantId(c), requireId(c), v);
        service::common::setRevisionEtag(c, v + 1);
        co_return c.json(service::common::operation(c, "证书已删除"));
    }
};
} // namespace service::certificate
