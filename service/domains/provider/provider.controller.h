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
#include "service/domains/provider/certificate_provider.schema.h"
#include "service/domains/provider/certificate_provider.service.h"
#include "service/domains/provider/dns_provider.schema.h"
#include "service/domains/provider/dns_provider.service.h"
#include "service/middleware/auth.h"
#include "service/features/live_resource/fanout.h"
#include "service/features/live_resource/sse.h"
namespace service::provider {
class ProviderController final : public ruvia::Controller<ProviderController> {
  public:
    RUVIA_CONTROLLER_GROUP("/api/providers", service::middleware::AuthMiddleware)
    RUVIA_ROUTES_BEGIN RUVIA_GET_SSE("/dns/:id/stream", getDns);
    RUVIA_GET_SSE("/dns/stream", listDns);
    RUVIA_GET_SSE("/dns/options/stream", optionsDns);
    RUVIA_POST("/dns", createDns, CreateDnsProviderValidator);
    RUVIA_PUT("/dns/:id", updateDns, UpdateDnsProviderValidator);
    RUVIA_POST("/dns/:id/verify", verifyDns);
    RUVIA_DELETE("/dns/:id", removeDns);
    RUVIA_GET_SSE("/certificate/stream", listCertificates);
    RUVIA_GET_SSE("/certificate/options/stream", listCertificates);
    RUVIA_POST("/certificate", createCertificate, CreateCertificateProviderValidator);
    RUVIA_PUT("/certificate/:id", updateCertificate, UpdateCertificateProviderValidator);
    RUVIA_POST("/certificate/:id/verify", verifyCertificate);
    RUVIA_DELETE("/certificate/:id", removeCertificate);
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
    ruvia::Task<void> optionsDns(ruvia::Context& c) {
        const auto keyword = service::common::requireKeyword(c.req().query("keyword"));
        std::optional<std::string> status;
        if (const auto value = c.req().query("status")) {
            if (*value != "unverified" && *value != "verified" && *value != "invalid")
                service::common::throwAppError(service::common::kValidationErrorCode,
                                               "status 不正确", 400);
            status.emplace(*value);
        }
        const auto tenant = tenantId(c);
        auto sub = service::live_resource::hub().subscribe(
            c.worker(), tenant, service::live_resource::Resource::providers, {},
            service::live_resource::queryKey("dns-options", keyword.value_or(""), status));
        co_await service::live_resource::streamSnapshot(
            c, std::move(sub),
            [tenant, keyword, status](ruvia::WebWorkerContext& read) -> ruvia::Task<std::string> {
                ruvia::Array<DnsProviderDto> values(read.resource());
                for (std::int64_t pageNumber = 1;; ++pageNumber) {
                    auto page = co_await dnsProviderReadService().list(
                        read, tenant, pageNumber, 1000, (pageNumber - 1) * 1000, keyword, status);
                    for (auto& item : page.ensure<"list">())
                        values.push_back(std::move(item));
                    if (pageNumber >= page.get<"totalPages">().value)
                        break;
                }
                auto response =
                    service::common::ok<DnsProviderOptionsResponse>(read, std::move(values));
                const auto json = ruvia::toJson(response, {.resource = read.resource()});
                co_return std::string(json.data(), json.size());
            });
    }
    ruvia::Task<void> listDns(ruvia::Context& c) {
        auto [page, size, skip] = service::common::requirePagination(c);
        auto keyword = service::common::requireKeyword(c.req().query("keyword"));
        std::optional<std::string> status;
        if (auto v = c.req().query("status")) {
            if (*v != "unverified" && *v != "verified" && *v != "invalid")
                service::common::throwAppError(service::common::kValidationErrorCode,
                                               "status 不正确", 400);
            status.emplace(*v);
        }
        auto tenant = tenantId(c);
        auto sub = service::live_resource::hub().subscribe(
            c.worker(), tenant, service::live_resource::Resource::providers, {},
            service::live_resource::queryKey("dns-list", page, size, keyword.value_or(""),
                                             status));
        co_await service::live_resource::streamSnapshot(
            c, std::move(sub),
            [tenant, page, size, skip, keyword,
             status](ruvia::WebWorkerContext& r) -> ruvia::Task<std::string> {
                auto data = co_await dnsProviderReadService().list(r, tenant, page, size, skip,
                                                                   keyword, status);
                auto response = service::common::ok<DnsProviderPageResponse>(r, std::move(data));
                auto json = ruvia::toJson(response, {.resource = r.resource()});
                co_return std::string(json.data(), json.size());
            });
    }
    ruvia::Task<void> getDns(ruvia::Context& c) {
        auto id = requireId(c);
        auto tenant = tenantId(c);
        auto sub = service::live_resource::hub().subscribe(
            c.worker(), tenant, service::live_resource::Resource::providers, id,
            service::live_resource::queryKey("dns-detail"));
        co_await service::live_resource::streamSnapshot(
            c, std::move(sub),
            [tenant, id](ruvia::WebWorkerContext& r) -> ruvia::Task<std::string> {
                auto data = co_await dnsProviderReadService().get(r, tenant, id);
                auto response = service::common::ok<DnsProviderDetailResponse>(r, std::move(data));
                auto json = ruvia::toJson(response, {.resource = r.resource()});
                co_return std::string(json.data(), json.size());
            });
    }
    ruvia::Task<void> listCertificates(ruvia::Context& c) {
        auto tenant = tenantId(c);
        auto sub = service::live_resource::hub().subscribe(
            c.worker(), tenant, service::live_resource::Resource::providers, {},
            service::live_resource::queryKey("certificate-list"));
        co_await service::live_resource::streamSnapshot(
            c, std::move(sub), [tenant](ruvia::WebWorkerContext& r) -> ruvia::Task<std::string> {
                auto data = co_await certificateProviderReadService().list(r, tenant);
                auto response =
                    service::common::ok<CertificateProviderListResponse>(r, std::move(data));
                auto json = ruvia::toJson(response, {.resource = r.resource()});
                co_return std::string(json.data(), json.size());
            });
    }
    ruvia::Task<ruvia::HttpResponse> createDns(ruvia::Context& c) {
        co_await dnsProviderService().create(c, tenantId(c),
                                             c.req().validated<CreateDnsProviderBody>());
        service::common::setRevisionEtag(c, 1);
        co_return c.json(service::common::operation(c, "DNS 服务商账号已添加"));
    }
    ruvia::Task<ruvia::HttpResponse> updateDns(ruvia::Context& c) {
        auto v = expectedRevision(c);
        co_await dnsProviderService().update(c, tenantId(c), requireId(c), v,
                                             c.req().validated<UpdateDnsProviderBody>());
        service::common::setRevisionEtag(c, v + 1);
        co_return c.json(service::common::operation(c, "DNS 服务商账号已更新"));
    }
    ruvia::Task<ruvia::HttpResponse> verifyDns(ruvia::Context& c) {
        auto v = expectedRevision(c);
        co_await dnsProviderService().verifyStored(c, tenantId(c), requireId(c), v);
        service::common::setRevisionEtag(c, v);
        co_return c.json(service::common::operation(c, "DNS 服务商凭据检测任务已提交"));
    }
    ruvia::Task<ruvia::HttpResponse> removeDns(ruvia::Context& c) {
        auto v = expectedRevision(c);
        co_await dnsProviderService().remove(c, tenantId(c), requireId(c), v);
        service::common::setRevisionEtag(c, v + 1);
        co_return c.json(service::common::operation(c, "DNS 服务商账号已移除"));
    }
    ruvia::Task<ruvia::HttpResponse> createCertificate(ruvia::Context& c) {
        co_await certificateProviderService().create(
            c, tenantId(c), c.req().validated<CreateCertificateProviderBody>());
        service::common::setRevisionEtag(c, 1);
        co_return c.json(service::common::operation(c, "证书供应商已保存"));
    }
    ruvia::Task<ruvia::HttpResponse> updateCertificate(ruvia::Context& c) {
        auto v = expectedRevision(c);
        co_await certificateProviderService().update(
            c, tenantId(c), requireId(c), v, c.req().validated<UpdateCertificateProviderBody>());
        service::common::setRevisionEtag(c, v + 1);
        co_return c.json(service::common::operation(c, "证书供应商已更新"));
    }
    ruvia::Task<ruvia::HttpResponse> verifyCertificate(ruvia::Context& c) {
        auto v = expectedRevision(c);
        co_await certificateProviderService().verify(c, tenantId(c), requireId(c), v);
        service::common::setRevisionEtag(c, v);
        co_return c.json(service::common::operation(c, "证书供应商检测任务已提交"));
    }
    ruvia::Task<ruvia::HttpResponse> removeCertificate(ruvia::Context& c) {
        auto v = expectedRevision(c);
        co_await certificateProviderService().remove(c, tenantId(c), requireId(c), v);
        service::common::setRevisionEtag(c, v + 1);
        co_return c.json(service::common::operation(c, "证书供应商已删除"));
    }
};
} // namespace service::provider
