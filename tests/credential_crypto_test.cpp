#include <algorithm>
#include <cctype>
#include <iostream>
#include <stdexcept>
#include <source_location>
#include <string>
#include <string_view>
#include "service/utils/token.h"
#include "service/utils/password.h"
#include "service/domains/auth/session_credential.h"

int main() {
    try {
        const auto require = [](bool condition,
                                std::source_location location = std::source_location::current()) {
            if (!condition) {
                throw std::runtime_error(std::string(location.file_name()) + ":" +
                                         std::to_string(location.line()) + ": check failed");
            }
        };
        const auto firstToken = service::utils::randomToken();
        const service::auth::SessionCredential credential{
            "session-id", service::utils::SensitiveString("secret.value")};
        const auto encoded = service::auth::encodeSessionCredential(credential);
        const auto decoded = service::auth::parseSessionCredential(encoded.view());
        require(decoded.has_value());
        require(decoded->id == credential.id && decoded->secret.view() == credential.secret.view());
        for (const auto invalid : {"", "missing-separator", ".secret", "id."}) {
            require(!service::auth::parseSessionCredential(invalid));
        }
        const auto secondToken = service::utils::randomToken();
        require(firstToken.size() == 64);
        require(std::ranges::all_of(firstToken, [](unsigned char ch) { return std::isxdigit(ch); }));
        require(firstToken != secondToken);
        constexpr std::string_view abcDigest =
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
        require(service::utils::tokenHash("abc") == abcDigest);
        require(service::utils::tokenHashMatches("abc", abcDigest));
        require(!service::utils::tokenHashMatches("abd", abcDigest));
        const auto passwordHash = service::utils::hashPassword("test-password");
        require(passwordHash.starts_with("pbkdf2_sha256$210000$"));
        require(service::utils::comparePassword("test-password", passwordHash));
        require(!service::utils::comparePassword("wrong-password", passwordHash));
        require(!service::utils::comparePassword("test-password", passwordHash + "z"));

        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
