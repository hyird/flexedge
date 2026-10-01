#pragma once

#include <ruvia/web/Controller.h>

#include "service/common/domain_name.h"
#include "service/common/uuid.h"
#include "service/features/certificate/config_mapper.h"
#include "service/domains/certificate/certificate.types.h"

namespace service::certificate {

class CreateCertificateValidator final : public ruvia::Middleware {
public:
    ruvia::Task<void> handle(ruvia::Context& context, ruvia::Next& next) {
        const auto& body = context.req().validatedJson<CreateCertificateBody>().value();
        ruvia::Validator validator({.resource = context.pool()});
        const auto& domain = body.get<"domain">();
        validator.required(domain, "domain", "域名不能为空");
        if (domain) {
            validator.minLength(domain, "domain", 1, "域名不能为空");
            validator.maxLength(domain, "domain", 253, "域名最多253个字符");
            if (!service::common::isHostname(domain->view())) {
                validator.add("domain", "regex", "域名格式不正确");
            }
        }

        const auto& providerId = body.get<"certificateProviderId">();
        validator.required(providerId, "certificate_provider_id", "请选择证书供应商");
        if (providerId && !service::common::parseUuid(std::optional<std::string_view>{providerId->view()})) {
            validator.add("certificate_provider_id", "regex", "证书供应商不正确");
        }

        const auto& dnsZoneId = body.get<"dnsZoneId">();
        validator.required(dnsZoneId, "dns_zone_id", "请选择托管域名");
        if (dnsZoneId && !service::common::parseUuid(std::optional<std::string_view>{dnsZoneId->view()})) {
            validator.add("dns_zone_id", "regex", "托管域名不正确");
        }

        const auto& config = body.get<"config">();
        validator.required(config, "config", "证书配置不能为空");
        if (config && !service::certificate_issuance::normalize(*config).has_value()) {
            validator.add("config", "custom", "证书配置不正确");
        }
        std::move(validator).throwIfInvalid();
        co_await next();
    }
};

class CertificateConfigValidator final : public ruvia::Middleware {
public:
    ruvia::Task<void> handle(ruvia::Context& context, ruvia::Next& next) {
        const auto& body = context.req().validatedJson<service::certificate_issuance::CertificateConfigInput>().value();
        ruvia::Validator validator({.resource = context.pool()});
        service::certificate_issuance::validateCertificateConfig(body, validator);
        std::move(validator).throwIfInvalid();
        co_await next();
    }
};

} // namespace service::certificate
