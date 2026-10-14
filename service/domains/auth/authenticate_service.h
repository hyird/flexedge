#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/db/db.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "service/domains/auth/auth_user_store.h"
#include "service/domains/auth/login_throttle_store.h"
#include "service/utils/password.h"

namespace service::auth {

struct login_locked final {
    std::int64_t remaining_seconds_;
};
struct login_rejected final {
    int remaining_attempts_;
};
struct login_user_disabled final {};
using login_failure = std::variant<login_locked, login_rejected, login_user_disabled>;

inline ruvia::task<std::variant<auth_user_record, login_failure>> authenticate_user(
    ruvia::db_handle db, const std::string& username, std::string_view password) {
    if (const auto remaining = co_await remaining_login_lock_seconds(db, username); remaining > 0) {
        co_return login_failure{login_locked{remaining}};
    }
    auto admin = co_await find_login_user(db, username);
    const auto password_matches = service::utils::compare_password(
        password, admin ? std::string_view(admin->password_hash_) : service::utils::dummy_password_hash);
    if (!admin || !password_matches) {
        const int failure_count = co_await record_login_failure(db, username);
        co_return login_failure{login_rejected{5 - failure_count}};
    }
    if (admin->user_.status_ == "disabled") {
        co_return login_failure{login_user_disabled{}};
    }
    co_await clear_login_failures(db, username);
    co_return std::move(admin->user_);
}

}  // namespace service::auth
