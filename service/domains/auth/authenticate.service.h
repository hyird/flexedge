#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <ruvia/core/Task.h>
#include <ruvia/web/db/Db.h>
#include "service/domains/auth/auth_user.store.h"
#include "service/domains/auth/login_throttle.store.h"
#include "service/utils/password.h"

namespace service::auth {

struct LoginLocked final { std::int64_t remainingSeconds; };
struct LoginRejected final { int remainingAttempts; };
struct LoginUserDisabled final {};
using LoginFailure = std::variant<LoginLocked, LoginRejected, LoginUserDisabled>;

inline ruvia::Task<std::expected<AuthUserRecord, LoginFailure>> authenticateUser(
    ruvia::DbHandle db, const std::string& username, std::string_view password) {
    if (const auto remaining = co_await remainingLoginLockSeconds(db, username); remaining > 0) {
        co_return std::unexpected(LoginFailure{LoginLocked{remaining}});
    }
    auto admin = co_await findLoginUser(db, username);
    const auto passwordMatches = service::utils::comparePassword(
        password, admin ? std::string_view(admin->passwordHash) : service::utils::kDummyPasswordHash);
    if (!admin || !passwordMatches) {
        const int failureCount = co_await recordLoginFailure(db, username);
        co_return std::unexpected(LoginFailure{LoginRejected{5 - failureCount}});
    }
    if (admin->user.status == "disabled") {
        co_return std::unexpected(LoginFailure{LoginUserDisabled{}});
    }
    co_await clearLoginFailures(db, username);
    co_return std::move(admin->user);
}

} // namespace service::auth
