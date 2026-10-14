#pragma once

#include <cstdint>

#include "ruvia/web/model.h"

namespace service::auth {

RUVIA_MODEL(login_body,
    RUVIA_OPTIONAL_FIELD(username, ruvia::string),
    RUVIA_OPTIONAL_FIELD(password, ruvia::string));

RUVIA_MODEL(auth_user_info_dto, RUVIA_REQUIRED_FIELD(id, ruvia::string),
    RUVIA_REQUIRED_FIELD(username, ruvia::string),
    RUVIA_OPTIONAL_FIELD(nickname, ruvia::string, RUVIA_OMIT_EMPTY),
    RUVIA_REQUIRED_FIELD(status, ruvia::string));

RUVIA_MODEL(auth_session_dto, RUVIA_REQUIRED_FIELD(user, auth_user_info_dto));

RUVIA_MODEL(auth_session_response, RUVIA_REQUIRED_FIELD(code, ruvia::int64),
    RUVIA_REQUIRED_FIELD(message, ruvia::string),
    RUVIA_REQUIRED_FIELD(data, auth_session_dto));

RUVIA_MODEL(current_user_response, RUVIA_REQUIRED_FIELD(code, ruvia::int64),
    RUVIA_REQUIRED_FIELD(message, ruvia::string),
    RUVIA_REQUIRED_FIELD(data, auth_user_info_dto));

}  // namespace service::auth
