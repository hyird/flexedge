#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

#include <openssl/crypto.h>

#include "node/proto/credential_validation.h"
#include "node/runtime/secret_buffer.h"
#include "node/runtime/secure_file.h"

namespace flexedge::node {

class node_credentials final {
public:
    static node_credentials load(const std::filesystem::path& path) {
        auto bytes = read_secure_file(path);
        if (!bytes) {
            throw std::runtime_error("node credentials file does not exist");
        }
        secret_string_guard bytes_cleanser(*bytes);
        std::string node_id;
        std::string secret;
        secret_string_guard secret_cleanser(secret);
        bool has_node_id = false;
        bool has_secret = false;
        std::size_t offset{};
        while (offset < bytes->size()) {
            const auto end = bytes->find('\n', offset);
            const auto line = std::string_view(*bytes).substr(
                offset, end == std::string::npos ? bytes->size() - offset : end - offset);
            offset = end == std::string::npos ? bytes->size() : end + 1;
            if (line.empty()) {
                continue;
            }
            const auto separator = line.find('=');
            if (separator == std::string_view::npos) {
                throw std::runtime_error("invalid node credentials file");
            }
            const auto name = line.substr(0, separator);
            const auto value = line.substr(separator + 1);
            if (name == "node_id" && !has_node_id) {
                has_node_id = true;
                node_id.assign(value);
            } else if (name == "secret" && !has_secret) {
                has_secret = true;
                secret.assign(value);
            } else {
                throw std::runtime_error("invalid node credentials file field");
            }
        }
        if (!valid_credential_node_id(node_id) || !valid_credential_secret(secret)) {
            throw std::runtime_error("node credentials file contains invalid values");
        }
        return node_credentials(std::move(node_id), std::move(secret));
    }

    node_credentials(node_credentials&& other) noexcept
        : node_id_(std::move(other.node_id_)),
          secret_(std::move(other.secret_)) {
        cleanse(other.secret_);
    }

    node_credentials& operator=(node_credentials&&) = delete;
    node_credentials(const node_credentials&) = delete;
    node_credentials& operator=(const node_credentials&) = delete;
    ~node_credentials() {
        cleanse(secret_);
    }

    [[nodiscard]] const std::string& node_id() const noexcept {
        return node_id_;
    }
    [[nodiscard]] std::string_view secret() const noexcept {
        return secret_;
    }

private:
    node_credentials(std::string node_id, std::string secret)
        : node_id_(std::move(node_id)),
          secret_(std::move(secret)) {}

    static void cleanse(std::string& value) noexcept {
        OPENSSL_cleanse(value.data(), value.size());
        value.clear();
    }

    std::string node_id_;
    std::string secret_;
};

}  // namespace flexedge::node
