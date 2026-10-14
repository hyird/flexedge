#pragma once

#include "service/common/http.h"

namespace service::provider {

struct certificate_provider_error {
    static inline constexpr service::common::app_error_def not_found{16005, "证书供应商不存在", 404};
    static inline constexpr service::common::app_error_def exists{16006, "该类型的证书供应商已经存在",
        409};
    static inline constexpr service::common::app_error_def email_required{
        16007, "证书供应商账户邮箱不能为空", 422};
    static inline constexpr service::common::app_error_def in_use{
        16009, "证书供应商正在使用，不能删除", 409};
    static inline constexpr service::common::app_error_def access_key_required{
        16014, "ZeroSSL API Access Key 不能为空", 422};
    static inline constexpr service::common::app_error_def revision_conflict{
        16016, "配置已被其他请求修改，请刷新后重试", 412};
};

}  // namespace service::provider
