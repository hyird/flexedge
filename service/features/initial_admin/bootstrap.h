#pragma once

#include <ruvia/core/asio_task.h>
#include <ruvia/core/event_loop_pool.h>
#include <ruvia/core/task.h>
#include <ruvia/web/db/db_client.h>
#include <ruvia/web/db/db_handle.h>
#include <ruvia/web/db/db_transaction.h>
#include <ruvia/web/dotenv.h>

#include <future>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <asio/co_spawn.hpp>
#include <asio/use_future.hpp>

#include "service/utils/password.h"
#include "service/utils/sensitive_string.h"

namespace service::initial_admin {

inline constexpr std::string_view username{"admin"};
inline constexpr std::string_view nickname{"Administrator"};
inline constexpr std::string_view tenant_name{"FlexEdge"};

class initial_admin_config final {
public:
    explicit initial_admin_config(service::utils::sensitive_string password)
        : password_(std::move(password)) {}

    initial_admin_config(const initial_admin_config&) = delete;
    initial_admin_config& operator=(const initial_admin_config&) = delete;
    initial_admin_config(initial_admin_config&&) noexcept = default;
    initial_admin_config& operator=(initial_admin_config&&) noexcept = default;

    [[nodiscard]] std::string_view password() const noexcept {
        return password_.view();
    }

private:
    service::utils::sensitive_string password_;
};

inline std::optional<initial_admin_config> get_initial_admin_config(const ruvia::env& env) {
    const auto value = env.get("INITIAL_ADMIN_PASSWORD");
    if (!value || value->empty()) {
        return std::nullopt;
    }
    if (value->size() < 8 || value->size() > 128) {
        throw std::runtime_error(
            "INITIAL_ADMIN_PASSWORD must contain between 8 and 128 characters");
    }
    return initial_admin_config(service::utils::sensitive_string(std::string(*value)));
}

inline ruvia::task<void> initialize(ruvia::db_client& database,
    std::optional<initial_admin_config> config) {
    co_await database.connect();
    auto db = database.handle();
    auto transaction = co_await db.begin_transaction();
    const auto state = co_await transaction.query(
        "SELECT initialized_at IS NOT NULL FROM sys_instance_state WHERE singleton = true FOR "
        "UPDATE");
    if (state.empty()) {
        throw std::runtime_error("instance initialization state is missing");
    }
    if (state.front()[0].as<bool>().value_or(false)) {
        co_await transaction.commit();
        co_return;
    }
    if (!config) {
        throw std::runtime_error(
            "INITIAL_ADMIN_PASSWORD is required before the first service startup");
    }

    const auto password_hash = service::utils::hash_password(config->password());
    (void)co_await transaction.execute(
        "INSERT INTO sys_tenant (name, slug, status) VALUES ($1, "
        "CONCAT('tenant-', LEFT(gen_random_uuid()::text, 12)), 'enabled')",
        tenant_name);
    (void)co_await transaction.execute(
        "INSERT INTO sys_admin (username, password_hash, nickname, status) VALUES "
        "($1, $2, $3, 'enabled')",
        username, password_hash, nickname);
    (void)co_await transaction.execute(
        "UPDATE sys_instance_state SET initialized_at = CURRENT_TIMESTAMP WHERE singleton = true");
    co_await transaction.commit();
}

inline void initialize(const ruvia::db_config& database_config,
    std::optional<initial_admin_config> config) {
    ruvia::event_loop_pool loops({.loop_count_ = 1, .queue_capacity_ = 64});
    ruvia::db_client database(loops.loop(0), database_config);
    auto completion = asio::co_spawn(loops.loop(0).executor(),
        ruvia::as_awaitable(initialize(database, std::move(config))),
        asio::use_future);
    try {
        loops.start();
        completion.get();
        database.close();
        loops.stop();
        loops.join();
    } catch (...) {
        database.close();
        loops.stop();
        try {
            loops.join();
        } catch (...) {
        }
        throw;
    }
}

}  // namespace service::initial_admin
