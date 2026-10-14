#pragma once

#include "service/common/http.h"

namespace service::website {

struct website_error {
    static inline constexpr service::common::app_error_def not_found{16601, "网站不存在", 404};
    static inline constexpr service::common::app_error_def domain_exists{16602,
        "该域名已绑定其他网站", 409};
    static inline constexpr service::common::app_error_def cluster_unavailable{16604,
        "所属集群不可用", 422};
    static inline constexpr service::common::app_error_def certificate_unavailable{
        16609, "所选证书不可用", 422};
    static inline constexpr service::common::app_error_def managed_zone_not_found{
        16611, "该域名不属于当前租户的托管域名", 422};
    static inline constexpr service::common::app_error_def managed_zone_apex_unsupported{
        16612, "托管根域名不能创建 CNAME，请绑定子域名或选择外部解析", 422};
    static inline constexpr service::common::app_error_def revision_conflict{
        16614, "配置已被其他请求修改，请刷新后重试", 412};
    static inline constexpr service::common::app_error_def https_certificate_selection_invalid{
        16615, "HTTPS 开启时必须选择证书，关闭时不能绑定证书", 422};
};

}  // namespace service::website
