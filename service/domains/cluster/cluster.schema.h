#pragma once

#include <ruvia/web/Controller.h>

#include "service/common/uuid.h"
#include "service/domains/cluster/cluster.types.h"

namespace service::cluster {

class SaveClusterValidator final : public ruvia::Middleware {
public:
    ruvia::Task<void> handle(ruvia::Context& context, ruvia::Next& next) {
        const auto& body = context.req().validatedJson<SaveClusterBody>().value();
        ruvia::Validator validator({.resource = context.pool()});
        const auto& name = body.get<"name">();
        validator.required(name, "name", "集群名称不能为空");
        if (name) {
            validator.minLength(name, "name", 1, "集群名称不能为空");
            validator.maxLength(name, "name", 100, "集群名称最多100个字符");
            if (!hasNonWhitespace(*name)) validator.add("name", "regex", "集群名称不能为空");
        }

        const auto& dnsZoneId = body.get<"dnsZoneId">();
        validator.required(dnsZoneId, "dns_zone_id", "请选择托管域名");
        if (dnsZoneId && !isValidUuid(*dnsZoneId)) {
            validator.add("dns_zone_id", "regex", "托管域名不正确");
        }

        const auto& hostnamePrefix = body.get<"hostnamePrefix">();
        validator.required(hostnamePrefix, "hostname_prefix", "主机前缀不能为空");
        if (hostnamePrefix) {
            validator.minLength(hostnamePrefix, "hostname_prefix", 1, "主机前缀不能为空");
            validator.maxLength(hostnamePrefix, "hostname_prefix", 63, "主机前缀最多63个字符");
            if (!isValidHostnamePrefix(*hostnamePrefix)) {
                validator.add("hostname_prefix", "regex", "主机前缀格式不正确");
            }
        }

        const auto& status = body.get<"status">();
        validator.required(status, "status", "集群状态不能为空");
        if (status && status->view() != "enabled" && status->view() != "disabled") {
            validator.add("status", "regex", "集群状态不正确");
        }
        std::move(validator).throwIfInvalid();
        co_await next();
    }
};

} // namespace service::cluster
