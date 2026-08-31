#pragma once

#include "service/common/http.h"

namespace service::dns_zone {

struct DnsZoneError {
    static inline constexpr service::common::AppErrorDef NOT_FOUND{16201, "托管域名不存在", 404};
    static inline constexpr service::common::AppErrorDef EXISTS{16202, "该域名已经托管", 409};
    static inline constexpr service::common::AppErrorDef IN_USE{
        16203, "域名仍被证书、集群或网站使用", 409};
    static inline constexpr service::common::AppErrorDef PROVIDER_NOT_FOUND{
        16204, "DNS 服务商账号不存在", 404};
    static inline constexpr service::common::AppErrorDef REVISION_CONFLICT{
        16205, "配置已被其他请求修改，请刷新后重试", 412};
    static inline constexpr service::common::AppErrorDef DNS_LINE_INVALID{
        16206, "DNS 记录使用了不可用的解析线路", 422};
    static inline constexpr service::common::AppErrorDef PROVIDER_UNAVAILABLE{
        16207, "DNS 服务商账号不可用", 422};
    static inline constexpr service::common::AppErrorDef CLUSTER_MANAGED_RECORD{
        16208, "集群托管域名的 A、AAAA 或 CNAME 记录由节点自动维护", 422};
};

} // namespace service::dns_zone
