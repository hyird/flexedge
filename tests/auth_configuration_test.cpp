#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <ruvia/web/App.h>
#include "service/config/auth.h"

#define REQUIRE(condition) do { if (!(condition)) throw std::runtime_error(#condition); } while (false)

template <typename Function> bool throwsRuntimeError(Function&& function) {
    try { function(); } catch (const std::exception&) { return true; }
    return false;
}

int main() {
    try {
    {
        const ruvia::Env empty;
        const auto defaults = service::config::authConfiguration(empty);
        REQUIRE(defaults.sessionExpiresIn == std::chrono::hours(24 * 7));
        REQUIRE(defaults.cookieSecure);
        const auto path = std::filesystem::temp_directory_path() /
            ("flexedge-auth-config-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()) + ".env");
        const auto load = [&](std::string_view contents) {
            {
                std::ofstream file(path);
                file << contents;
                file.close();
                REQUIRE(!file.fail());
            }
            try {
                ruvia::app().loadDotenv(path, {
                    .existingVariables = ruvia::DotenvExistingVariablePolicy::kOverride});
            } catch (...) {
                std::filesystem::remove(path);
                throw;
            }
            std::filesystem::remove(path);
        };
        load("AUTH_SESSION_EXPIRES_IN=2h\nAUTH_COOKIE_SECURE=false\n");
        const auto overridden = service::config::authConfiguration(ruvia::app().env());
        REQUIRE(overridden.sessionExpiresIn == std::chrono::hours(2));
        REQUIRE(!overridden.cookieSecure);
        for (const auto invalid : {"TRUE", "1", "yes", ""}) {
            load("AUTH_SESSION_EXPIRES_IN=2h\nAUTH_COOKIE_SECURE=" + std::string(invalid) + "\n");
            REQUIRE(throwsRuntimeError([&] {
                (void)service::config::authConfiguration(ruvia::app().env());
            }));
        }
        load("AUTH_SESSION_EXPIRES_IN=0s\nAUTH_COOKIE_SECURE=true\n");
        REQUIRE(throwsRuntimeError([&] {
            (void)service::config::authConfiguration(ruvia::app().env());
        }));
    }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
