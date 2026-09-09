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

class NodeCredentials final {
  public:
    static NodeCredentials load(const std::filesystem::path& path) {
        auto bytes = readSecureFile(path);
        if (!bytes) {
            throw std::runtime_error("node credentials file does not exist");
        }
        SecretStringGuard bytesCleanser(*bytes);
        std::string nodeId;
        std::string secret;
        SecretStringGuard secretCleanser(secret);
        bool hasNodeId = false;
        bool hasSecret = false;
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
            if (name == "node_id" && !hasNodeId) {
                hasNodeId = true;
                nodeId.assign(value);
            } else if (name == "secret" && !hasSecret) {
                hasSecret = true;
                secret.assign(value);
            } else {
                throw std::runtime_error("invalid node credentials file field");
            }
        }
        if (!validCredentialNodeId(nodeId) || !validCredentialSecret(secret)) {
            throw std::runtime_error("node credentials file contains invalid values");
        }
        return NodeCredentials(std::move(nodeId), std::move(secret));
    }

    NodeCredentials(NodeCredentials&& other) noexcept
        : nodeId_(std::move(other.nodeId_)), secret_(std::move(other.secret_)) {
        cleanse(other.secret_);
    }

    NodeCredentials& operator=(NodeCredentials&&) = delete;
    NodeCredentials(const NodeCredentials&) = delete;
    NodeCredentials& operator=(const NodeCredentials&) = delete;
    ~NodeCredentials() { cleanse(secret_); }

    [[nodiscard]] const std::string& nodeId() const noexcept { return nodeId_; }
    [[nodiscard]] std::string_view secret() const noexcept { return secret_; }

  private:
    NodeCredentials(std::string nodeId, std::string secret)
        : nodeId_(std::move(nodeId)), secret_(std::move(secret)) {}

    static void cleanse(std::string& value) noexcept {
        OPENSSL_cleanse(value.data(), value.size());
        value.clear();
    }

    std::string nodeId_;
    std::string secret_;
};

} // namespace flexedge::node
