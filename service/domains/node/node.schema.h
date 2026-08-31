#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_set>

#include <ruvia/web/Controller.h>

#include "service/common/http.h"
#include "service/common/ip_address.h"
#include "service/common/types.h"
#include "service/domains/node/node.types.h"

namespace service::node {

inline constexpr std::size_t kMaxNodeIpAddresses{8};

inline bool
hasUniqueEndpoints(const ruvia::Array<service::node_config::NodeEndpointInput>& values) {
    std::unordered_set<std::string_view> ids;
    std::unordered_set<std::string_view> addresses;
    ids.reserve(values.size());
    addresses.reserve(values.size());
    for (const auto& value : values) {
        const auto& id = value.get<"id">();
        const auto& address = value.get<"ipAddress">();
        if (!id || !address || !ids.insert(id->view()).second ||
            !addresses.insert(address->view()).second) {
            return false;
        }
    }
    return true;
}

struct EndpointValidator final {
    template <typename ValidatorT>
    void validateNested(const service::node_config::NodeEndpointInput& value, std::string_view path,
                        ValidatorT& validator) const {
        const auto& id = value.get<"id">();
        if (!id || !service::common::parseUuid(id ? std::optional<std::string_view>{id->view()}
                                                  : std::nullopt)) {
            validator.add(std::string(path) + ".id", "format", "Endpoint ID 不正确");
        }

        const auto& ipAddress = value.get<"ipAddress">();
        const auto ipPath = std::string(path) + ".ip_address";
        if (!ipAddress || ipAddress->empty()) {
            validator.add(ipPath, "required", "IP 地址不能为空");
        } else if (ipAddress->size() > 45 || !service::common::isIpAddress(ipAddress->view())) {
            validator.add(ipPath, "format", "IP 地址格式不正确");
        }

        const auto& lineCode = value.get<"lineCode">();
        const auto linePath = std::string(path) + ".line_code";
        if (!lineCode || lineCode->empty()) {
            validator.add(linePath, "required", "请选择 DNS 线路");
        } else if (lineCode->size() > 64) {
            validator.add(linePath, "too_big", "DNS 线路不正确");
        }
    }
};

struct NodeConfigRules final {
    void validateNested(const service::node_config::NodeConfigInput& body, std::string_view prefix,
                        ruvia::Validator& validator) const {
        RUVIA_VALIDATION_FOR_EACH(
            RUVIA_VALIDATE_RULE_FIELD, service::node_config::NodeConfigInput,
            RUVIA_RULE(endpoints, RUVIA_REQUIRED("请至少填写一个 IP"),
                       RUVIA_MIN(1, "请至少填写一个 IP"),
                       RUVIA_MAX(kMaxNodeIpAddresses, "最多配置8个 IP"),
                       RUVIA_CUSTOM("Endpoint ID 和 IP 地址不能重复", hasUniqueEndpoints),
                       RUVIA_EACH(EndpointValidator)))
    }
};

class NodeConfigValidator final : public ruvia::Middleware<NodeConfigValidator> {
    RUVIA_VALIDATE_JSON(
        NodeSaveInput,
        RUVIA_RULE_NAME("cluster_id", clusterId, RUVIA_REQUIRED("请选择所属集群"),
                        RUVIA_REGEX("所属集群不正确", service::common::kUuidPattern)),
        RUVIA_RULE(name, RUVIA_REQUIRED("节点名称不能为空"), RUVIA_MIN(1, "节点名称不能为空"),
                   RUVIA_MAX(100, "节点名称最多100个字符"),
                   RUVIA_REGEX("节点名称不能为空", R"(^.*\S.*$)")),
        RUVIA_RULE(status, RUVIA_REQUIRED("节点状态不能为空"),
                   RUVIA_REGEX("节点状态不正确", R"(^(enabled|disabled)$)")),
        RUVIA_RULE(config, RUVIA_REQUIRED("节点配置不能为空"), RUVIA_NESTED(NodeConfigRules)))
};

} // namespace service::node
