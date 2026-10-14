#pragma once

#include <ruvia/web/controller.h>

#include "service/common/uuid.h"
#include "service/domains/cluster/cluster_types.h"

namespace service::cluster {

class save_cluster_validator final {
public:
    ruvia::task<void> handle(ruvia::context& context, ruvia::next& next) {
        const auto& body = context.req().validated_json<save_cluster_body>().value();
        ruvia::validator validator({.resource_ = context.pool()});
        const auto& name = body.get<"name">();
        validator.required(name, "name", "集群名称不能为空");
        if (name) {
            validator.min_length(name, "name", 1, "集群名称不能为空");
            validator.max_length(name, "name", 100, "集群名称最多100个字符");
            if (!has_non_whitespace(*name)) {
                validator.add("name", "regex", "集群名称不能为空");
            }
        }

        const auto& dns_zone_id = body.get<"dns_zone_id">();
        validator.required(dns_zone_id, "dns_zone_id", "请选择托管域名");
        if (dns_zone_id && !is_valid_uuid(*dns_zone_id)) {
            validator.add("dns_zone_id", "regex", "托管域名不正确");
        }

        const auto& hostname_prefix = body.get<"hostname_prefix">();
        validator.required(hostname_prefix, "hostname_prefix", "主机前缀不能为空");
        if (hostname_prefix) {
            validator.min_length(hostname_prefix, "hostname_prefix", 1, "主机前缀不能为空");
            validator.max_length(hostname_prefix, "hostname_prefix", 63, "主机前缀最多63个字符");
            if (!is_valid_hostname_prefix(*hostname_prefix)) {
                validator.add("hostname_prefix", "regex", "主机前缀格式不正确");
            }
        }

        const auto& status = body.get<"status">();
        validator.required(status, "status", "集群状态不能为空");
        if (status && status->view() != "enabled" && status->view() != "disabled") {
            validator.add("status", "regex", "集群状态不正确");
        }
        validator.throw_if_invalid();
        co_await next();
    }
};

}  // namespace service::cluster
