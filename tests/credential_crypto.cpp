#include <algorithm>
#include <cctype>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>

#include "service/domains/auth/session_credential.h"
#include "service/utils/password.h"
#include "service/utils/token.h"

int main() {
    try {
        const auto require = [](bool condition,
                                 std::source_location location = std::source_location::current()) {
            if (!condition) {
                throw std::runtime_error(std::string(location.file_name()) + ":" +
                                         std::to_string(location.line()) + ": check failed");
            }
        };
        const auto first_token = service::utils::random_token();
        const service::auth::session_credential credential{
            "session-id", service::utils::sensitive_string("secret.value")};
        const auto encoded = service::auth::encode_session_credential(credential);
        const auto decoded = service::auth::parse_session_credential(encoded.view());
        require(decoded.has_value());
        require(decoded->id_ == credential.id_ && decoded->secret_.view() == credential.secret_.view());
        for (const auto invalid : {"", "missing-separator", ".secret", "id."}) {
            require(!service::auth::parse_session_credential(invalid));
        }
        const auto second_token = service::utils::random_token();
        require(first_token.size() == 64);
        require(std::ranges::all_of(first_token, [](unsigned char ch) { return std::isxdigit(ch); }));
        require(first_token != second_token);
        constexpr std::string_view abc_digest =
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
        require(service::utils::token_hash("abc") == abc_digest);
        require(service::utils::token_hash_matches("abc", abc_digest));
        require(!service::utils::token_hash_matches("abd", abc_digest));
        const auto password_hash = service::utils::hash_password("test-password");
        require(password_hash.starts_with("pbkdf2_sha256$210000$"));
        require(service::utils::compare_password("test-password", password_hash));
        require(!service::utils::compare_password("wrong-password", password_hash));
        require(!service::utils::compare_password("test-password", password_hash + "z"));

        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
