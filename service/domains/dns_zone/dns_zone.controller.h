#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <ruvia/core/Task.h>
#include <ruvia/web/Context.h>
#include <ruvia/web/Controller.h>

#include "service/common/domain_name.h"
#include "service/common/http.h"
#include "service/domains/dns_zone/dns_zone.schema.h"
#include "service/domains/dns_zone/dns_zone.service.h"
#include "service/middleware/auth.h"

namespace service::dns_zone {

class DnsZoneController final : public ruvia::Controller<DnsZoneController> {
  public:
    RUVIA_CONTROLLER_GROUP("/api/dns-zones", service::middleware::AuthMiddleware)
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/available", available);
    RUVIA_GET("/options", options);
    RUVIA_POST("/:id/sync", sync);
    RUVIA_GET("/:id", get);
    RUVIA_PUT("/:id", update, DnsZoneConfigValidator);
    RUVIA_GET("/", list);
    RUVIA_POST("/", create, CreateDnsZoneValidator);
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

    ruvia::Task<ruvia::HttpResponse> available(ruvia::Context& c) {
        const auto providerId = service::common::parseUuid(c.req().query("dns_provider_id"));
        if (!providerId) {
            service::common::throwAppError(service::common::kValidationErrorCode,
                                           "dns_provider_id 必须是 UUID", 400);
        }
        co_return c.json(service::common::ok<AvailableDnsZoneListResponse>(
            c, co_await dnsZoneService().available(c, tenantId(c), *providerId)));
    }

    ruvia::Task<ruvia::HttpResponse> list(ruvia::Context& c) {
        const auto [page, pageSize, skip] = service::common::requirePagination(c);
        const auto keyword = service::common::requireKeyword(c.req().query("keyword"));
        std::optional<std::string> providerId;
        if (const auto value = c.req().query("dns_provider_id")) {
            providerId = service::common::parseUuid(value);
            if (!providerId) {
                service::common::throwAppError(service::common::kValidationErrorCode,
                                               "dns_provider_id 必须是 UUID", 400);
            }
        }
        co_return c.json(service::common::ok<DnsZonePageResponse>(
            c, co_await dnsZoneService().list(c, tenantId(c), page, pageSize, skip, keyword,
                                              providerId)));
    }

    ruvia::Task<ruvia::HttpResponse> options(ruvia::Context& c) {
        const auto keyword = service::common::requireKeyword(c.req().query("keyword"));
        std::optional<std::string> ownerOf;
        if (const auto value = c.req().query("owner_of")) {
            if (!service::common::isHostname(*value)) {
                service::common::throwAppError(service::common::kValidationErrorCode,
                                               "owner_of 域名不正确", 400);
            }
            ownerOf.emplace(*value);
        }
        std::optional<bool> available;
        if (const auto value = c.req().query("available")) {
            available = service::common::parseBoolean(value);
            if (!available) {
                service::common::throwAppError(service::common::kValidationErrorCode,
                                               "available 必须是 true 或 false", 400);
            }
        }
        co_return c.json(service::common::ok<DnsZoneOptionListResponse>(
            c, co_await dnsZoneService().options(c, tenantId(c), keyword, ownerOf, available)));
    }

    ruvia::Task<ruvia::HttpResponse> create(ruvia::Context& c) {
        co_await dnsZoneService().create(c, tenantId(c), c.req().validated<CreateDnsZoneBody>());
        service::common::setRevisionEtag(c, 1);
        co_return c.json(service::common::operation(c, "域名已保存，同步任务已提交"));
    }

    ruvia::Task<ruvia::HttpResponse> get(ruvia::Context& c) {
        auto data = co_await dnsZoneService().get(c, tenantId(c), requireId(c));
        service::common::setRevisionEtag(c, data.get<"revision">().value);
        co_return c.json(service::common::ok<DnsZoneDetailResponse>(c, std::move(data)));
    }

    ruvia::Task<ruvia::HttpResponse> update(ruvia::Context& c) {
        const auto revision = expectedRevision(c);
        co_await dnsZoneService().updateConfig(
            c, tenantId(c), requireId(c), revision,
            c.req().validatedJson<service::dns_sync::ZoneConfigInput>());
        service::common::setRevisionEtag(c, revision + 1);
        co_return c.json(service::common::operation(c, "配置已保存，同步任务已提交"));
    }

    ruvia::Task<ruvia::HttpResponse> sync(ruvia::Context& c) {
        std::string_view conflictPolicy;
        if (auto body = co_await c.req().jsonIf<DnsZoneSyncBody>()) {
            ruvia::Validator validator({.resource = c.resource()});
            validateDnsZoneSync(*body, validator);
            validator.throwIfInvalid({.resource = c.resource()});
            if (const auto& value = body->get<"conflictPolicy">()) {
                conflictPolicy = value->view();
            }
        }
        co_await dnsZoneService().requestSync(c, tenantId(c), requireId(c), conflictPolicy);
        co_return c.json(service::common::operation(c, "同步任务已提交"));
    }

    ruvia::Task<ruvia::HttpResponse> remove(ruvia::Context& c) {
        const auto revision = expectedRevision(c);
        co_await dnsZoneService().remove(c, tenantId(c), requireId(c), revision);
        service::common::setRevisionEtag(c, revision + 1);
        co_return c.json(service::common::operation(c, "托管域名已移除"));
    }
};

} // namespace service::dns_zone
