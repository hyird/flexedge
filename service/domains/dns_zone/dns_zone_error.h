#pragma once

#include "service/common/http.h"

namespace service::dns_zone {

struct dns_zone_error {
    static inline constexpr service::common::app_error_def not_found{16201, "托管域名不存在", 404};
    static inline constexpr service::common::app_error_def exists{16202, "该域名已经托管", 409};
    static inline constexpr service::common::app_error_def in_use{
        16203, "域名仍被证书、集群或网站使用", 409};
    static inline constexpr service::common::app_error_def provider_not_found{
        16204, "DNS 服务商账号不存在", 404};
    static inline constexpr service::common::app_error_def revision_conflict{
        16205, "配置已被其他请求修改，请刷新后重试", 412};
    static inline constexpr service::common::app_error_def dns_line_invalid{
        16206, "DNS 记录使用了不可用的解析线路", 422};
    static inline constexpr service::common::app_error_def provider_unavailable{
        16207, "DNS 服务商账号不可用", 422};
    static inline constexpr service::common::app_error_def system_managed_record{
        16208, "系统自动生成的 DNS 记录不能修改或覆盖", 422};
};

}  // namespace service::dns_zone
