#pragma once

#include "service/common/http.h"

namespace service::provider {

struct DnsProviderError {
    static inline constexpr service::common::AppErrorDef NOT_FOUND{16101, "DNS 服务商账号不存在",
                                                                   404};
    static inline constexpr service::common::AppErrorDef NAME_EXISTS{16102, "账号名称已存在", 409};
    static inline constexpr service::common::AppErrorDef ACCOUNT_EXISTS{16103, "该账户已经存在",
                                                                        409};
    static inline constexpr service::common::AppErrorDef IN_USE{
        16104, "账号下仍有托管域名或尚未完成的 DNS 清理", 409};
    static inline constexpr service::common::AppErrorDef REVISION_CONFLICT{
        16107, "配置已被其他请求修改，请刷新后重试", 412};
};

} // namespace service::provider
