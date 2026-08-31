#pragma once

#include "service/common/http.h"

namespace service::auth {

struct AuthError {
    static inline constexpr service::common::AppErrorDef USER_NOT_FOUND{11001, "用户不存在", 400};
    static inline constexpr service::common::AppErrorDef USER_DISABLED{11002, "用户已被禁用", 403};
    static inline constexpr service::common::AppErrorDef PASSWORD_INCORRECT{
        11003, "用户名或密码错误", 401};
    static inline constexpr service::common::AppErrorDef UNAUTHORIZED{
        service::common::kAuthUnauthorizedErrorCode, "未登录", 401};
    static inline constexpr service::common::AppErrorDef SESSION_INVALID{
        service::common::kAuthSessionInvalidErrorCode, "会话已失效", 401};
    static inline constexpr service::common::AppErrorDef TOO_MANY_ATTEMPTS{
        11008, "登录失败次数过多，账户已被锁定", 429};
}; // struct AuthError

} // namespace service::auth
