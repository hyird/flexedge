#pragma once
#include <google/protobuf/util/json_util.h>
#include <ruvia/web/model_json.h>

#include "node/proto/security_validation.h"
#include "node/proto/edge_control.pb.h"
#include "service/features/security_config/transport.h"

namespace service::security_config {
// Storage/API and the node protocol remain separate contracts. This conversion
// rejects unknown fields and overflows rather than silently coercing settings.
inline flexedge::node::v2::SecurityPolicy to_proto(const policy_dto& config,
    const std::string& id, const std::string& name, std::int64_t revision) {
    flexedge::node::v2::SecurityPolicy result;
    if (!google::protobuf::util::JsonStringToMessage(ruvia::to_json(config), &result).ok()) {
        throw std::invalid_argument("安全规则配置格式不正确");
    }
    result.set_id(id);
    result.set_name(name);
    result.set_revision(revision);
    flexedge::node::validate_security(result);
    return result;
}
inline policy_dto parse(std::string_view json, ruvia::model_options options = {}) {
    auto parsed = ruvia::from_json<policy_dto>(json, {.resource_ = options.resource_});
    if (!parsed) {
        throw std::runtime_error("invalid stored security configuration");
    }
    return std::move(*parsed);
}
}  // namespace service::security_config
