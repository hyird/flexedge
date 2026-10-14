#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/app.h>
#include <ruvia/web/context.h>

#include "service/common/http.h"
#include "service/config/auth.h"
#include "service/domains/auth/auth_error.h"
#include "service/domains/auth/auth_session_store.h"
#include "service/domains/auth/auth_types.h"
#include "service/domains/auth/auth_user_mapper.h"
#include "service/domains/auth/session_cookie.h"

namespace service::auth {

class auth_session_service final {
public:
    ruvia::task<void> start(ruvia::context& c, const std::string& admin_id) const {
        const auto configuration = service::config::get_auth_configuration(ruvia::app().env());
        const auto session = co_await create_session_credential(c.db(), admin_id, configuration.session_expires_in_);
        set_session_cookie(c, session);
    }

    ruvia::task<auth_session_dto> refresh(ruvia::context& c) const {
        const auto presented = read_session_cookie(c);
        if (!presented) {
            service::common::throw_app_error(auth_error::unauthorized);
        }
        const auto configuration = service::config::get_auth_configuration(ruvia::app().env());
        const auto rotation = co_await rotate_session(c.db(), *presented, configuration.session_expires_in_);
        if (const auto* error = std::get_if<session_rotation_error>(&rotation)) {
            if (*error != session_rotation_error::unknown_session) {
                delete_session_cookie(c);
            }
            service::common::throw_app_error(*error == session_rotation_error::user_disabled
                                                 ? auth_error::user_disabled
                                                 : auth_error::session_invalid);
        }
        const auto& rotated = std::get<rotated_session>(rotation);
        set_session_cookie(c, rotated.credential_);
        auth_session_dto result({.resource_ = c.pool()});
        result.set<"user">(auth_user_info(c, rotated.user_.id_, rotated.user_.username_,
            rotated.user_.nickname_, rotated.user_.status_));
        co_return result;
    }

    ruvia::task<void> logout(ruvia::context& c) const {
        const auto presented = read_session_cookie(c);
        if (presented) {
            co_await revoke_session_family(c.db(), *presented);
        }
        delete_session_cookie(c);
    }
};

inline const auth_session_service& get_auth_session_service() {
    static const auth_session_service service;
    return service;
}

}  // namespace service::auth
