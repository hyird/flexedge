#include <ruvia/web/app.h>

#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#include "service/config/auth.h"

#define REQUIRE(condition)                                      \
    do {                                                        \
        if (!(condition)) throw std::runtime_error(#condition); \
    } while (false)

template <typename function_type>
bool throws_runtime_error(function_type&& function) {
    try {
        function();
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

int main() {
    try {
        {
            const ruvia::env empty;
            const auto defaults = service::config::get_auth_configuration(empty);
            REQUIRE(defaults.session_expires_in_ == std::chrono::hours(24 * 7));
            REQUIRE(defaults.cookie_secure_);
            const auto path = std::filesystem::temp_directory_path() /
                              ("flexedge-auth-config-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".env");
            const auto load = [&](std::string_view contents) {
                {
                    std::ofstream file(path);
                    file << contents;
                    file.close();
                    REQUIRE(!file.fail());
                }
                try {
                    ruvia::app().load_dotenv(path, {.existing_variables_ = ruvia::dotenv_existing_variable_policy::override});
                } catch (...) {
                    std::filesystem::remove(path);
                    throw;
                }
                std::filesystem::remove(path);
            };
            load("AUTH_SESSION_EXPIRES_IN=2h\nAUTH_COOKIE_SECURE=false\n");
            const auto overridden = service::config::get_auth_configuration(ruvia::app().env());
            REQUIRE(overridden.session_expires_in_ == std::chrono::hours(2));
            REQUIRE(!overridden.cookie_secure_);
            for (const auto invalid : {"TRUE", "1", "yes", ""}) {
                load("AUTH_SESSION_EXPIRES_IN=2h\nAUTH_COOKIE_SECURE=" + std::string(invalid) + "\n");
                REQUIRE(throws_runtime_error([&] {
                    (void)service::config::get_auth_configuration(ruvia::app().env());
                }));
            }
            load("AUTH_SESSION_EXPIRES_IN=0s\nAUTH_COOKIE_SECURE=true\n");
            REQUIRE(throws_runtime_error([&] {
                (void)service::config::get_auth_configuration(ruvia::app().env());
            }));
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
