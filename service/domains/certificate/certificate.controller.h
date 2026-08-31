#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <ruvia/core/Task.h>
#include <ruvia/web/Context.h>
#include <ruvia/web/Controller.h>

#include "service/common/http.h"
#include "service/common/types.h"
#include "service/domains/certificate/certificate.schema.h"
#include "service/domains/certificate/certificate.service.h"
#include "service/middleware/auth.h"

namespace service::certificate {

class CertificateController final : public ruvia::Controller<CertificateController> {
  public:
    RUVIA_CONTROLLER_GROUP("/api/certificates", service::middleware::AuthMiddleware)
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/", list);
    RUVIA_POST("/", create, CreateCertificateValidator);
    RUVIA_POST("/:id/renew", renew);
    RUVIA_GET("/:id/download", download);
    RUVIA_GET("/:id", get);
    RUVIA_PUT("/:id", update, CertificateConfigValidator);
    RUVIA_DELETE("/:id", remove);
    RUVIA_ROUTES_END

  private:
    static std::string requireId(ruvia::Context& c) {
        return service::common::requireUuidParam(c, "id");
    }

    static const std::string& tenantId(ruvia::Context& c) {
        return service::middleware::currentTenantId(c);
    }

    static std::int64_t expectedRevision(ruvia::Context& c) {
        return service::common::requireExpectedRevision(c);
    }

    static std::optional<std::string_view> requireStatus(ruvia::Context& c) {
        const auto status = c.req().query("status");
        if (status && *status != "pending" && *status != "issuing" && *status != "valid" &&
            *status != "renewing" && *status != "failed" && *status != "expired") {
            service::common::throwAppError(service::common::kValidationErrorCode, "证书状态不正确",
                                           400);
        }
        return status;
    }

    static std::optional<bool> requireUsable(ruvia::Context& c) {
        const auto value = c.req().query("usable");
        if (!value) {
            return std::nullopt;
        }
        const auto usable = service::common::parseBoolean(value);
        if (!usable) {
            service::common::throwAppError(service::common::kValidationErrorCode,
                                           "usable 必须是 true 或 false", 400);
        }
        return usable;
    }

    ruvia::Task<ruvia::HttpResponse> list(ruvia::Context& c) {
        const auto [page, pageSize, skip] = service::common::requirePagination(c);
        const auto keyword = service::common::requireKeyword(c.req().query("keyword"));
        co_return c.json(service::common::ok<CertificatePageResponse>(
            c, co_await certificateService().list(c, tenantId(c), page, pageSize, skip, keyword,
                                                  requireStatus(c), requireUsable(c))));
    }

    ruvia::Task<ruvia::HttpResponse> create(ruvia::Context& c) {
        co_await certificateService().create(c, tenantId(c),
                                             c.req().validated<CreateCertificateBody>());
        service::common::setRevisionEtag(c, 1);
        co_return c.json(service::common::operation(c, "证书申请已提交"));
    }

    ruvia::Task<ruvia::HttpResponse> get(ruvia::Context& c) {
        auto data = co_await certificateService().get(c, tenantId(c), requireId(c));
        service::common::setRevisionEtag(c, data.get<"revision">().value);
        co_return c.json(service::common::ok<CertificateDetailResponse>(c, std::move(data)));
    }

    ruvia::Task<ruvia::HttpResponse> update(ruvia::Context& c) {
        const auto revision = expectedRevision(c);
        co_await certificateService().update(
            c, tenantId(c), requireId(c), revision,
            c.req().validatedJson<service::certificate_issuance::CertificateConfigInput>());
        service::common::setRevisionEtag(c, revision + 1);
        co_return c.json(service::common::operation(c, "证书设置已更新"));
    }

    ruvia::Task<ruvia::HttpResponse> renew(ruvia::Context& c) {
        const auto revision = expectedRevision(c);
        co_await certificateService().renew(c, tenantId(c), requireId(c), revision);
        service::common::setRevisionEtag(c, revision);
        co_return c.json(service::common::operation(c, "重新签发已提交"));
    }

    ruvia::Task<ruvia::HttpResponse> download(ruvia::Context& c) {
        auto data = co_await certificateService().download(c, tenantId(c), requireId(c));
        c.header("cache-control", "no-store");
        c.header("content-disposition", "attachment; filename=\"" + data.filename + "\"");
        c.header("content-type", "application/zip");
        c.header("x-content-type-options", "nosniff");
        co_return c.body(std::string_view(data.archive));
    }

    ruvia::Task<ruvia::HttpResponse> remove(ruvia::Context& c) {
        const auto revision = expectedRevision(c);
        co_await certificateService().remove(c, tenantId(c), requireId(c), revision);
        service::common::setRevisionEtag(c, revision + 1);
        co_return c.json(service::common::operation(c, "证书已删除"));
    }
};

} // namespace service::certificate
