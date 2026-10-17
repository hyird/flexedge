#include <ruvia/web/app.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

#include "service/config/runtime_env.h"

#define REQUIRE(condition)                                      \
    do {                                                        \
        if (!(condition)) throw std::runtime_error(#condition); \
    } while (false)

int main() {
    const auto path = std::filesystem::temp_directory_path() /
                      ("flexedge-runtime-secrets-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".env");
    try {
        {
            std::ofstream file(path);
            file << "EXISTING_VALUE=preserved";
        }
        const ruvia::env empty;
        REQUIRE(service::config::ensure_runtime_secrets(path, empty));
        const auto read = [&] {
            std::ifstream file(path, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(file), {});
        };
        const auto generated = read();
        REQUIRE(generated.starts_with("EXISTING_VALUE=preserved\n"));
        ruvia::app().load_dotenv(path);
        const auto key = ruvia::app().env().get("SECRET_MASTER_KEY");
        REQUIRE(key && key->size() == 64);
        REQUIRE(key->find_first_not_of("0123456789abcdef") == std::string_view::npos);
        REQUIRE(ruvia::app().env().get("EXISTING_VALUE") == "preserved");
        REQUIRE(!service::config::ensure_runtime_secrets(path, ruvia::app().env()));
        REQUIRE(read() == generated);
#ifndef _WIN32
        const auto permissions = std::filesystem::status(path).permissions();
        REQUIRE((permissions & (std::filesystem::perms::group_all | std::filesystem::perms::others_all)) == std::filesystem::perms::none);
#endif
        std::filesystem::remove(path);
        return 0;
    } catch (const std::exception& error) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
