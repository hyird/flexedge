#pragma once

#include "service/common/http.h"

namespace service::provider {

struct CertificateProviderError {
    static inline constexpr service::common::AppErrorDef NOT_FOUND{16005, "证书供应商不存在", 404};
    static inline constexpr service::common::AppErrorDef EXISTS{16006, "该类型的证书供应商已经存在",
                                                                409};
    static inline constexpr service::common::AppErrorDef EMAIL_REQUIRED{
        16007, "证书供应商账户邮箱不能为空", 422};
    static inline constexpr service::common::AppErrorDef IN_USE{
        16009, "证书供应商正在使用，不能删除", 409};
    static inline constexpr service::common::AppErrorDef ACCESS_KEY_REQUIRED{
        16014, "ZeroSSL API Access Key 不能为空", 422};
    static inline constexpr service::common::AppErrorDef REVISION_CONFLICT{
        16016, "配置已被其他请求修改，请刷新后重试", 412};
};

} // namespace service::provider
