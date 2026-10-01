#pragma once

#include <ruvia/web/Controller.h>

#include "service/domains/dns_zone/dns_zone.types.h"

namespace service::dns_zone {

class CreateDnsZoneValidator final : public ruvia::Middleware {
public:
    ruvia::Task<void> handle(ruvia::Context& context, ruvia::Next& next) {
        const auto& body = context.req().validatedJson<CreateDnsZoneBody>().value();
        ruvia::Validator validator({.resource = context.pool()});
        const auto& providerId = body.get<"dnsProviderId">();
        validator.required(providerId, "dns_provider_id", "请选择 DNS 服务商账号");
        if (providerId && !isValidUuid(*providerId)) {
            validator.add("dns_provider_id", "regex", "DNS 服务商账号不正确");
        }

        const auto& domain = body.get<"domain">();
        validator.required(domain, "domain", "域名不能为空");
        if (domain) {
            validator.minLength(domain, "domain", 1, "域名不能为空");
            validator.maxLength(domain, "domain", 253, "域名最多253个字符");
            if (!isValidZoneDomain(*domain)) validator.add("domain", "regex", "域名格式不正确");
        }
        std::move(validator).throwIfInvalid();
        co_await next();
    }
};

class DnsZoneConfigValidator final : public ruvia::Middleware {
public:
    ruvia::Task<void> handle(ruvia::Context& context, ruvia::Next& next) {
        const auto& body = context.req().validatedJson<service::dns_sync::ZoneConfigInput>().value();
        ruvia::Validator validator({.resource = context.pool()});
        service::dns_sync::validateZoneConfig(body, validator);
        std::move(validator).throwIfInvalid();
        co_await next();
    }
};

inline void validateDnsZoneSync(const DnsZoneSyncBody& body, ruvia::Validator& validator) {
    const auto& policy = body.get<"conflictPolicy">();
    if (policy && policy->view() != "local" && policy->view() != "remote") {
        validator.add("conflict_policy", "one_of", "冲突处理方式不正确");
    }
}

} // namespace service::dns_zone
