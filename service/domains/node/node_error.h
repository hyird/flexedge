#pragma once

#include "service/common/http.h"

namespace service::node {

struct node_error {
    static inline constexpr service::common::app_error_def not_found{16501, "节点不存在", 404};
    static inline constexpr service::common::app_error_def exists{16502, "节点名称或 IP 地址已存在",
        409};
    static inline constexpr service::common::app_error_def cluster_unavailable{16503,
        "所属集群不可用", 422};
    static inline constexpr service::common::app_error_def dns_line_invalid{
        16504, "DNS 线路不属于所选集群", 422};
    static inline constexpr service::common::app_error_def revision_conflict{
        16505, "配置已被其他请求修改，请刷新后重试", 412};
    static inline constexpr service::common::app_error_def credentials_unavailable{
        16506, "节点持久化凭据不可用，请重置凭据", 409};
};

}  // namespace service::node
