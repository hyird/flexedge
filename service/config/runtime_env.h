#pragma once

#include "common/hex.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

#include <openssl/crypto.h>
#include <openssl/rand.h>

#include <ruvia/web/Dotenv.h>

#include "service/utils/sensitive_string.h"

namespace service::config {

namespace runtime_env_detail {

inline std::string randomHex(std::size_t byteCount) {
    std::string bytes(byteCount, '\0');
    const service::utils::SensitiveBufferGuard cleanseBytes(bytes);
    if (RAND_bytes(reinterpret_cast<unsigned char*>(bytes.data()),
                   static_cast<int>(bytes.size())) != 1) {
        throw std::runtime_error("failed to generate runtime secret");
    }

    return flexedge::crypto::hexEncode(
        {reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size()});
}

inline bool endsWithNewline(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("failed to read dotenv before generating runtime secrets");
    }
    input.seekg(0, std::ios::end);
    const auto size = input.tellg();
    if (size < 0) {
        throw std::runtime_error("failed to inspect dotenv size");
    }
    if (size == 0) {
        return true;
    }
    input.seekg(-1, std::ios::end);
    const auto last = input.get();
    if (!input) throw std::runtime_error("failed to read dotenv ending");
    return last == '\n';
}

inline void restrictPermissions(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::permissions(
        path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace, ec);
    if (ec) {
        throw std::runtime_error("failed to restrict dotenv permissions: " + ec.message());
    }
}

inline void writeGeneratedSecret(std::ofstream& output, std::string_view name,
                                 std::size_t byteCount) {
    service::utils::SensitiveString secret(randomHex(byteCount));
    output << name << '=' << secret.view() << '\n';
}

} // namespace runtime_env_detail

inline std::filesystem::path dotenvPath(const std::filesystem::path& runtimeDir) {
    auto workingDirectoryPath = std::filesystem::current_path() / ".env";
    if (std::filesystem::exists(workingDirectoryPath)) {
        return workingDirectoryPath;
    }
    return runtimeDir / ".env";
}

inline bool ensureRuntimeSecrets(const std::filesystem::path& path, const ruvia::Env& env) {
    const bool needsMasterKey = !env.get("SECRET_MASTER_KEY").has_value();
    if (!needsMasterKey) {
        runtime_env_detail::restrictPermissions(path);
        return false;
    }

    runtime_env_detail::restrictPermissions(path);
    std::ofstream output(path, std::ios::binary | std::ios::app);
    if (!output) {
        throw std::runtime_error("failed to open dotenv for generated runtime secrets");
    }

    if (!runtime_env_detail::endsWithNewline(path)) {
        output << '\n';
    }
    output << "\n# 首次启动自动生成，请勿提交或跨环境复用。\n";
    if (needsMasterKey) {
        runtime_env_detail::writeGeneratedSecret(output, "SECRET_MASTER_KEY", 32);
    }
    output.flush();
    if (!output) {
        throw std::runtime_error("failed to persist generated runtime secrets");
    }
    output.close();
    if (!output) {
        throw std::runtime_error("failed to close dotenv after generating runtime secrets");
    }
    runtime_env_detail::restrictPermissions(path);
    return true;
}

} // namespace service::config
