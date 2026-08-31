#pragma once

#include <cstdint>
#include <string>

#include <ruvia/core/Task.h>
#include <ruvia/web/Context.h>
#include <ruvia/web/Controller.h>

#include "service/common/http.h"
#include "service/common/types.h"
#include "service/domains/provider/certificate_provider.schema.h"
#include "service/domains/provider/certificate_provider.service.h"
#include "service/domains/provider/dns_provider.schema.h"
#include "service/domains/provider/dns_provider.service.h"
#include "service/middleware/auth.h"

namespace service::provider {

class ProviderController final : public ruvia::Controller<ProviderController> {
  public:
    RUVIA_CONTROLLER_GROUP("/api/providers", service::middleware::AuthMiddleware)
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/dns/:id", getDns);
    RUVIA_GET("/dns", listDns);
    RUVIA_POST("/dns", createDns, CreateDnsProviderValidator);
    RUVIA_PUT("/dns/:id", updateDns, UpdateDnsProviderValidator);
    RUVIA_POST("/dns/:id/verify", verifyDns);
    RUVIA_DELETE("/dns/:id", removeDns);
    RUVIA_GET("/certificate", listCertificates);
    RUVIA_POST("/certificate", createCertificate, CreateCertificateProviderValidator);
    RUVIA_PUT("/certificate/:id", updateCertificate, UpdateCertificateProviderValidator);
    RUVIA_POST("/certificate/:id/verify", verifyCertificate);
    RUVIA_DELETE("/certificate/:id", removeCertificate);
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

    ruvia::Task<ruvia::HttpResponse> listDns(ruvia::Context& c) {
        const auto [page, pageSize, skip] = service::common::requirePagination(c);
        const auto keyword = service::common::requireKeyword(c.req().query("keyword"));
        std::optional<std::string> status;
        if (const auto value = c.req().query("status")) {
            if (*value != "unverified" && *value != "verified" && *value != "invalid") {
                service::common::throwAppError(service::common::kValidationErrorCode,
                                               "status 不正确", 400);
            }
            status.emplace(*value);
        }
        co_return c.json(service::common::ok<DnsProviderPageResponse>(
            c, co_await dnsProviderService().list(c, tenantId(c), page, pageSize, skip, keyword,
                                                  status)));
    }

    ruvia::Task<ruvia::HttpResponse> getDns(ruvia::Context& c) {
        auto data = co_await dnsProviderService().get(c, tenantId(c), requireId(c));
        service::common::setRevisionEtag(c, data.get<"revision">().value);
        co_return c.json(service::common::ok<DnsProviderDetailResponse>(c, std::move(data)));
    }

    ruvia::Task<ruvia::HttpResponse> createDns(ruvia::Context& c) {
        co_await dnsProviderService().create(c, tenantId(c),
                                             c.req().validated<CreateDnsProviderBody>());
        service::common::setRevisionEtag(c, 1);
        co_return c.json(service::common::operation(c, "DNS 服务商账号已添加"));
    }

    ruvia::Task<ruvia::HttpResponse> updateDns(ruvia::Context& c) {
        const auto revision = expectedRevision(c);
        co_await dnsProviderService().update(c, tenantId(c), requireId(c), revision,
                                             c.req().validated<UpdateDnsProviderBody>());
        service::common::setRevisionEtag(c, revision + 1);
        co_return c.json(service::common::operation(c, "DNS 服务商账号已更新"));
    }

    ruvia::Task<ruvia::HttpResponse> verifyDns(ruvia::Context& c) {
        const auto revision = expectedRevision(c);
        co_await dnsProviderService().verifyStored(c, tenantId(c), requireId(c), revision);
        service::common::setRevisionEtag(c, revision);
        co_return c.json(service::common::operation(c, "DNS 服务商凭据检测任务已提交"));
    }

    ruvia::Task<ruvia::HttpResponse> removeDns(ruvia::Context& c) {
        const auto revision = expectedRevision(c);
        co_await dnsProviderService().remove(c, tenantId(c), requireId(c), revision);
        service::common::setRevisionEtag(c, revision + 1);
        co_return c.json(service::common::operation(c, "DNS 服务商账号已移除"));
    }

    ruvia::Task<ruvia::HttpResponse> listCertificates(ruvia::Context& c) {
        co_return c.json(service::common::ok<CertificateProviderListResponse>(
            c, co_await certificateProviderService().list(c, tenantId(c))));
    }

    ruvia::Task<ruvia::HttpResponse> createCertificate(ruvia::Context& c) {
        co_await certificateProviderService().create(
            c, tenantId(c), c.req().validated<CreateCertificateProviderBody>());
        service::common::setRevisionEtag(c, 1);
        co_return c.json(service::common::operation(c, "证书供应商已保存"));
    }

    ruvia::Task<ruvia::HttpResponse> updateCertificate(ruvia::Context& c) {
        const auto revision = expectedRevision(c);
        co_await certificateProviderService().update(
            c, tenantId(c), requireId(c), revision,
            c.req().validated<UpdateCertificateProviderBody>());
        service::common::setRevisionEtag(c, revision + 1);
        co_return c.json(service::common::operation(c, "证书供应商已更新"));
    }

    ruvia::Task<ruvia::HttpResponse> verifyCertificate(ruvia::Context& c) {
        const auto revision = expectedRevision(c);
        co_await certificateProviderService().verify(c, tenantId(c), requireId(c), revision);
        service::common::setRevisionEtag(c, revision);
        co_return c.json(service::common::operation(c, "证书供应商检测任务已提交"));
    }

    ruvia::Task<ruvia::HttpResponse> removeCertificate(ruvia::Context& c) {
        const auto revision = expectedRevision(c);
        co_await certificateProviderService().remove(c, tenantId(c), requireId(c), revision);
        service::common::setRevisionEtag(c, revision + 1);
        co_return c.json(service::common::operation(c, "证书供应商已删除"));
    }
};

} // namespace service::provider
