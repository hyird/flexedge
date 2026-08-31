#pragma once

#include <cstdint>
#include <ruvia/web/Model.h>

namespace service::auth {

RUVIA_REQUEST_MODEL(LoginBody, RUVIA_OPTIONAL_FIELD(username, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(password, ruvia::String));

RUVIA_RESPONSE_MODEL(AuthUserInfoDto, RUVIA_REQUIRED_FIELD(id, ruvia::String),
                     RUVIA_REQUIRED_FIELD(username, ruvia::String),
                     RUVIA_OPTIONAL_FIELD(nickname, ruvia::String, RUVIA_OMIT_EMPTY),
                     RUVIA_REQUIRED_FIELD(status, ruvia::String));

RUVIA_RESPONSE_MODEL(AuthSessionDto, RUVIA_REQUIRED_FIELD(user, AuthUserInfoDto));

RUVIA_RESPONSE_MODEL(AuthSessionResponse, RUVIA_REQUIRED_FIELD(code, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD(message, ruvia::String),
                     RUVIA_REQUIRED_FIELD(data, AuthSessionDto));

RUVIA_RESPONSE_MODEL(CurrentUserResponse, RUVIA_REQUIRED_FIELD(code, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD(message, ruvia::String),
                     RUVIA_REQUIRED_FIELD(data, AuthUserInfoDto));

} // namespace service::auth
