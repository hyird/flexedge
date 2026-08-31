#pragma once

#include "service/common/http.h"

namespace service::node {

struct NodeError {
    static inline constexpr service::common::AppErrorDef NOT_FOUND{16501, "节点不存在", 404};
    static inline constexpr service::common::AppErrorDef EXISTS{16502, "节点名称或 IP 地址已存在",
                                                                409};
    static inline constexpr service::common::AppErrorDef CLUSTER_UNAVAILABLE{16503,
                                                                             "所属集群不可用", 422};
    static inline constexpr service::common::AppErrorDef DNS_LINE_INVALID{
        16504, "DNS 线路不属于所选集群", 422};
    static inline constexpr service::common::AppErrorDef REVISION_CONFLICT{
        16505, "配置已被其他请求修改，请刷新后重试", 412};
    static inline constexpr service::common::AppErrorDef CREDENTIALS_UNAVAILABLE{
        16506, "节点持久化凭据不可用，请重置凭据", 409};
};

} // namespace service::node
