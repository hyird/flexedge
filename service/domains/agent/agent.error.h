#pragma once

#include "service/common/http.h"

namespace service::agent {

inline constexpr service::common::AppErrorDef AGENT_UNAUTHORIZED{16801, "Agent 凭据无效", 401};
inline constexpr service::common::AppErrorDef BOOTSTRAP_INVALID{16802, "节点安装令牌无效或已过期",
                                                                401};
inline constexpr service::common::AppErrorDef NODE_NOT_FOUND{16803, "节点不存在", 404};
inline constexpr service::common::AppErrorDef REVISION_INVALID{16804, "节点配置版本不正确", 409};
inline constexpr service::common::AppErrorDef ARTIFACT_INVALID{
    16805, "节点配置制品不存在或摘要不匹配", 409};
inline constexpr service::common::AppErrorDef AGENT_ALREADY_BOUND{16806, "Agent 已绑定其他节点",
                                                                  409};

} // namespace service::agent
