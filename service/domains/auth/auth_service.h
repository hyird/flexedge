#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/context.h>

#include <string>
#include <variant>

#include "service/common/http.h"
#include "service/common/types.h"
#include "service/domains/auth/auth_error.h"
#include "service/domains/auth/auth_session_service.h"
#include "service/domains/auth/auth_types.h"
#include "service/domains/auth/auth_user_mapper.h"
#include "service/domains/auth/auth_user_store.h"
#include "service/domains/auth/authenticate_service.h"

namespace service::auth {

class auth_service {
public:
    static auth_service& instance() {
        static auth_service svc;
        return svc;
    }

    ruvia::task<auth_session_dto> login(ruvia::context& c, const login_body& req) {
        const auto& username_input = req.get<"username">();
        const auto& password_input = req.get<"password">();
        if (!username_input || !password_input) {
            service::common::throw_app_error(service::common::validation_error_code,
                "用户名和密码不能为空", 400);
        }
        const std::string username(username_input->view());
        const auto password = password_input->view();

        const auto authenticated = co_await authenticate_user(c.db(), username, password);
        if (const auto* failure = std::get_if<login_failure>(&authenticated)) {
            if (const auto* locked = std::get_if<login_locked>(failure)) {
                const auto minutes = (locked->remaining_seconds_ + 59) / 60;
                service::common::throw_app_error(
                    auth_error::too_many_attempts.code_,
                    "登录失败次数过多，请" + std::to_string(minutes) + "分钟后再试", 429);
            }
            if (const auto* rejected = std::get_if<login_rejected>(failure)) {
                if (rejected->remaining_attempts_ > 0) {
                    service::common::throw_app_error(
                        auth_error::password_incorrect.code_,
                        "用户名或密码错误，还剩" + std::to_string(rejected->remaining_attempts_) + "次尝试机会", 401);
                }
                service::common::throw_app_error(auth_error::too_many_attempts);
            }
            service::common::throw_app_error(auth_error::user_disabled);
        }
        const auto& user = std::get<auth_user_record>(authenticated);
        co_await get_auth_session_service().start(c, user.id_);
        auth_session_dto result({.resource_ = c.pool()});
        result.set<"user">(auth_user_info(c, user.id_, user.username_,
            user.nickname_, user.status_));
        co_return result;
    }

    ruvia::task<auth_user_info_dto> get_current_user(ruvia::context& c, const std::string& admin_id) {
        const auto user = co_await find_auth_user(c.db(), admin_id);
        if (!user) {
            service::common::throw_app_error(auth_error::user_not_found);
        }
        if (user->status_ == "disabled") {
            service::common::throw_app_error(auth_error::user_disabled);
        }
        co_return auth_user_info(c, user->id_, user->username_, user->nickname_, user->status_);
    }

private:
    auth_service() = default;
};

inline auth_service& get_auth_service() {
    return auth_service::instance();
}

}  // namespace service::auth
