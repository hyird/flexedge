#pragma once

#include "service/common/http.h"

namespace service::node_channel {

inline constexpr service::common::app_error_def agent_unauthorized{16801, "Agent 凭据无效", 401};
inline constexpr service::common::app_error_def revision_invalid{16804, "节点配置版本不正确", 409};
inline constexpr service::common::app_error_def artifact_invalid{
    16805, "节点配置制品不存在或摘要不匹配", 409};

}  // namespace service::node_channel
