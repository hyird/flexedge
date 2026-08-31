#pragma once

#include "service/common/http.h"

namespace service::certificate {

struct CertificateError {
    static inline constexpr service::common::AppErrorDef NOT_FOUND{16001, "证书任务不存在", 404};
    static inline constexpr service::common::AppErrorDef DOMAIN_EXISTS{16002, "该域名已有证书任务",
                                                                       409};
    static inline constexpr service::common::AppErrorDef DOMAIN_UNAVAILABLE{16003, "托管域名不可用",
                                                                            422};
    static inline constexpr service::common::AppErrorDef DOMAIN_MISMATCH{
        16004, "证书域名不属于所选托管域名", 422};
    static inline constexpr service::common::AppErrorDef PROVIDER_UNAVAILABLE{
        16008, "证书供应商尚未通过检测", 422};
    static inline constexpr service::common::AppErrorDef CERTIFICATE_UNAVAILABLE{
        16011, "证书尚未签发完成", 409};
    static inline constexpr service::common::AppErrorDef TASK_ACTIVE{
        16012, "证书已有正在执行的签发流程", 409};
    static inline constexpr service::common::AppErrorDef DNS_PROVIDER_UNSUPPORTED{
        16015, "证书 DNS-01 不支持当前 DNS 服务商", 422};
    static inline constexpr service::common::AppErrorDef REVISION_CONFLICT{
        16016, "配置已被其他请求修改，请刷新后重试", 412};
    static inline constexpr service::common::AppErrorDef IN_USE{
        16017, "证书已被网站引用，请先解除 HTTPS 证书绑定", 409};
};

} // namespace service::certificate
