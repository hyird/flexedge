#pragma once

#include <ruvia/web/dotenv.h>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

#include <openssl/crypto.h>
#include <openssl/rand.h>

#include "common/hex.h"
#include "service/utils/sensitive_string.h"

namespace service::config {

namespace runtime_env_detail {

inline std::string random_hex(std::size_t byte_count) {
    std::string bytes(byte_count, '\0');
    const service::utils::sensitive_buffer_guard cleanse_bytes(bytes);
    if (RAND_bytes(reinterpret_cast<unsigned char*>(bytes.data()),
            static_cast<int>(bytes.size())) != 1) {
        throw std::runtime_error("failed to generate runtime secret");
    }

    return flexedge::crypto::hex_encode(
        {reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size()});
}

inline bool ends_with_newline(const std::filesystem::path& path) {
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
    if (!input) {
        throw std::runtime_error("failed to read dotenv ending");
    }
    return last == '\n';
}

inline void restrict_permissions(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::permissions(
        path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace, ec);
    if (ec) {
        throw std::runtime_error("failed to restrict dotenv permissions: " + ec.message());
    }
}

inline void write_generated_secret(std::ofstream& output, std::string_view name,
    std::size_t byte_count) {
    service::utils::sensitive_string secret(random_hex(byte_count));
    output << name << '=' << secret.view() << '\n';
}

}  // namespace runtime_env_detail

inline std::filesystem::path dotenv_path(const std::filesystem::path& runtime_dir) {
    auto working_directory_path = std::filesystem::current_path() / ".env";
    if (std::filesystem::exists(working_directory_path)) {
        return working_directory_path;
    }
    return runtime_dir / ".env";
}

inline bool ensure_runtime_secrets(const std::filesystem::path& path, const ruvia::env& env) {
    const bool needs_master_key = !env.get("SECRET_MASTER_KEY").has_value();
    if (!needs_master_key) {
        runtime_env_detail::restrict_permissions(path);
        return false;
    }

    runtime_env_detail::restrict_permissions(path);
    std::ofstream output(path, std::ios::binary | std::ios::app);
    if (!output) {
        throw std::runtime_error("failed to open dotenv for generated runtime secrets");
    }

    if (!runtime_env_detail::ends_with_newline(path)) {
        output << '\n';
    }
    output << "\n# 首次启动自动生成，请勿提交或跨环境复用。\n";
    if (needs_master_key) {
        runtime_env_detail::write_generated_secret(output, "SECRET_MASTER_KEY", 32);
    }
    output.flush();
    if (!output) {
        throw std::runtime_error("failed to persist generated runtime secrets");
    }
    output.close();
    if (!output) {
        throw std::runtime_error("failed to close dotenv after generating runtime secrets");
    }
    runtime_env_detail::restrict_permissions(path);
    return true;
}

}  // namespace service::config
