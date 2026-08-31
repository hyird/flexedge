#pragma once

#include <ruvia/web/Controller.h>

#include "service/common/types.h"
#include "service/domains/cluster/cluster.types.h"

namespace service::cluster {

class SaveClusterValidator final : public ruvia::Middleware<SaveClusterValidator> {
    RUVIA_VALIDATE_JSON(
        SaveClusterBody,
        RUVIA_RULE(name, RUVIA_REQUIRED("集群名称不能为空"), RUVIA_MIN(1, "集群名称不能为空"),
                   RUVIA_MAX(100, "集群名称最多100个字符"),
                   RUVIA_REGEX("集群名称不能为空", R"(^.*\S.*$)")),
        RUVIA_RULE_NAME("dns_zone_id", dnsZoneId, RUVIA_REQUIRED("请选择托管域名"),
                        RUVIA_REGEX("托管域名不正确", service::common::kUuidPattern)),
        RUVIA_RULE_NAME("hostname_prefix", hostnamePrefix, RUVIA_REQUIRED("主机前缀不能为空"),
                        RUVIA_MIN(1, "主机前缀不能为空"), RUVIA_MAX(63, "主机前缀最多63个字符"),
                        RUVIA_REGEX("主机前缀格式不正确",
                                    R"(^[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?$)")),
        RUVIA_RULE(status, RUVIA_REQUIRED("集群状态不能为空"),
                   RUVIA_REGEX("集群状态不正确", R"(^(enabled|disabled)$)")))
};

} // namespace service::cluster
