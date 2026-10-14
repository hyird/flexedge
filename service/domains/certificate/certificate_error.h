#pragma once

#include "service/common/http.h"

namespace service::certificate {

struct certificate_error {
    static inline constexpr service::common::app_error_def not_found{16001, "证书任务不存在", 404};
    static inline constexpr service::common::app_error_def domain_exists{16002, "该域名已有证书任务",
        409};
    static inline constexpr service::common::app_error_def domain_unavailable{16003, "托管域名不可用",
        422};
    static inline constexpr service::common::app_error_def domain_mismatch{
        16004, "证书域名不属于所选托管域名", 422};
    static inline constexpr service::common::app_error_def provider_unavailable{
        16008, "证书供应商尚未通过检测", 422};
    static inline constexpr service::common::app_error_def certificate_unavailable{
        16011, "证书尚未签发完成", 409};
    static inline constexpr service::common::app_error_def task_active{
        16012, "证书已有正在执行的签发流程", 409};
    static inline constexpr service::common::app_error_def dns_provider_unsupported{
        16015, "证书 DNS-01 不支持当前 DNS 服务商", 422};
    static inline constexpr service::common::app_error_def revision_conflict{
        16016, "配置已被其他请求修改，请刷新后重试", 412};
    static inline constexpr service::common::app_error_def in_use{
        16017, "证书已被网站引用，请先解除 HTTPS 证书绑定", 409};
};

}  // namespace service::certificate
