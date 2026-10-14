#pragma once

#include "service/common/http.h"

namespace service::provider {

struct dns_provider_error {
    static inline constexpr service::common::app_error_def not_found{16101, "DNS 服务商账号不存在",
        404};
    static inline constexpr service::common::app_error_def name_exists{16102, "账号名称已存在", 409};
    static inline constexpr service::common::app_error_def account_exists{16103, "该账户已经存在",
        409};
    static inline constexpr service::common::app_error_def in_use{
        16104, "账号下仍有托管域名或尚未完成的 DNS 清理", 409};
    static inline constexpr service::common::app_error_def revision_conflict{
        16107, "配置已被其他请求修改，请刷新后重试", 412};
};

}  // namespace service::provider
