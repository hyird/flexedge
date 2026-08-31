#pragma once

#include <future>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <asio/co_spawn.hpp>
#include <asio/use_future.hpp>

#include <ruvia/core/AsioTask.h>
#include <ruvia/core/EventLoopPool.h>
#include <ruvia/core/Task.h>
#include <ruvia/web/Dotenv.h>
#include <ruvia/web/db/DbClient.h>

#include "service/utils/password.h"
#include "service/utils/sensitive_string.h"

namespace service::initial_admin {

inline constexpr std::string_view kUsername{"admin"};
inline constexpr std::string_view kNickname{"Administrator"};
inline constexpr std::string_view kTenantName{"FlexEdge"};

class InitialAdminConfig final {
  public:
    explicit InitialAdminConfig(service::utils::SensitiveString password)
        : password_(std::move(password)) {}

    InitialAdminConfig(const InitialAdminConfig&) = delete;
    InitialAdminConfig& operator=(const InitialAdminConfig&) = delete;
    InitialAdminConfig(InitialAdminConfig&&) noexcept = default;
    InitialAdminConfig& operator=(InitialAdminConfig&&) noexcept = default;

    [[nodiscard]] std::string_view password() const noexcept { return password_.view(); }

  private:
    service::utils::SensitiveString password_;
};

inline std::optional<InitialAdminConfig> initialAdminConfig(const ruvia::Env& env) {
    const auto value = env.get("INITIAL_ADMIN_PASSWORD");
    if (!value || value->empty()) {
        return std::nullopt;
    }
    if (value->size() < 8 || value->size() > 128) {
        throw std::runtime_error(
            "INITIAL_ADMIN_PASSWORD must contain between 8 and 128 characters");
    }
    return InitialAdminConfig(service::utils::SensitiveString(std::string(*value)));
}

inline ruvia::Task<void> initialize(ruvia::DbClient& database,
                                    std::optional<InitialAdminConfig> config) {
    co_await database.connect();
    auto transaction = co_await database.beginTransaction();
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

    const auto passwordHash = service::utils::hashPassword(config->password());
    (void)co_await transaction.execute(
        "INSERT INTO sys_tenant (name, slug, status) VALUES ($1, "
        "CONCAT('tenant-', LEFT(gen_random_uuid()::text, 12)), 'enabled')",
        kTenantName);
    (void)co_await transaction.execute(
        "INSERT INTO sys_admin (username, password_hash, nickname, status) VALUES "
        "($1, $2, $3, 'enabled')",
        kUsername, passwordHash, kNickname);
    (void)co_await transaction.execute(
        "UPDATE sys_instance_state SET initialized_at = CURRENT_TIMESTAMP WHERE singleton = true");
    co_await transaction.commit();
}

inline void initialize(const ruvia::DbConfig& databaseConfig,
                       std::optional<InitialAdminConfig> config) {
    ruvia::EventLoopPool loops({.loopCount = 1, .mailboxCapacity = 64});
    ruvia::DbClient database(loops.loop(0), databaseConfig);
    auto completion = asio::co_spawn(loops.loop(0).executor(),
                                     ruvia::asAwaitable(initialize(database, std::move(config))),
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

} // namespace service::initial_admin
