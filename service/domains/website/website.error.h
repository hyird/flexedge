#pragma once

#include "service/common/http.h"

namespace service::website {

struct WebsiteError {
    static inline constexpr service::common::AppErrorDef NOT_FOUND{16601, "网站不存在", 404};
    static inline constexpr service::common::AppErrorDef DOMAIN_EXISTS{16602,
                                                                       "该域名已绑定其他网站", 409};
    static inline constexpr service::common::AppErrorDef CLUSTER_UNAVAILABLE{16604,
                                                                             "所属集群不可用", 422};
    static inline constexpr service::common::AppErrorDef CERTIFICATE_UNAVAILABLE{
        16609, "所选证书不可用", 422};
    static inline constexpr service::common::AppErrorDef MANAGED_ZONE_NOT_FOUND{
        16611, "该域名不属于当前租户的托管域名", 422};
    static inline constexpr service::common::AppErrorDef MANAGED_ZONE_APEX_UNSUPPORTED{
        16612, "托管根域名不能创建 CNAME，请绑定子域名或选择外部解析", 422};
    static inline constexpr service::common::AppErrorDef REVISION_CONFLICT{
        16614, "配置已被其他请求修改，请刷新后重试", 412};
    static inline constexpr service::common::AppErrorDef HTTPS_CERTIFICATE_SELECTION_INVALID{
        16615, "HTTPS 开启时必须选择证书，关闭时不能绑定证书", 422};
};

} // namespace service::website
