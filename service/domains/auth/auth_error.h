#pragma once

#include "service/common/http.h"

namespace service::auth {

struct auth_error {
    static inline constexpr service::common::app_error_def user_not_found{11001, "用户不存在", 400};
    static inline constexpr service::common::app_error_def user_disabled{11002, "用户已被禁用", 403};
    static inline constexpr service::common::app_error_def password_incorrect{
        11003, "用户名或密码错误", 401};
    static inline constexpr service::common::app_error_def unauthorized{
        service::common::auth_unauthorized_error_code, "未登录", 401};
    static inline constexpr service::common::app_error_def session_invalid{
        service::common::auth_session_invalid_error_code, "会话已失效", 401};
    static inline constexpr service::common::app_error_def too_many_attempts{
        11008, "登录失败次数过多，账户已被锁定", 429};
};  // struct auth_error

}  // namespace service::auth
