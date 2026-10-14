#pragma once

#include "service/common/http.h"

namespace service::cluster {

struct cluster_error {
    static inline constexpr service::common::app_error_def not_found{16401, "集群不存在", 404};
    static inline constexpr service::common::app_error_def exists{16402, "集群名称或接入域名已存在",
        409};
    static inline constexpr service::common::app_error_def dns_zone_unavailable{
        16403, "托管域名不可用", 422};
    static inline constexpr service::common::app_error_def has_nodes{16404, "集群仍包含节点", 409};
    static inline constexpr service::common::app_error_def revision_conflict{
        16405, "配置已被其他请求修改，请刷新后重试", 412};
    static inline constexpr service::common::app_error_def has_websites{16406, "集群仍承载网站", 409};
    static inline constexpr service::common::app_error_def website_cleanup_active{
        16407, "集群仍有尚未完成的网站删除任务", 409};
};

}  // namespace service::cluster
