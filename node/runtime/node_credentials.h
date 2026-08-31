#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>

#include <openssl/crypto.h>

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
            if (name == "node_id" && nodeId.empty()) {
                nodeId.assign(value);
            } else if (name == "secret" && secret.empty()) {
                secret.assign(value);
            } else {
                throw std::runtime_error("invalid node credentials file field");
            }
        }
        if (!validNodeId(nodeId) || !validSecret(secret)) {
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

    [[nodiscard]] static bool validNodeId(std::string_view value) noexcept {
        return value.size() == 32 && std::ranges::all_of(value, [](unsigned char ch) {
                   return std::isdigit(ch) != 0 || (ch >= 'a' && ch <= 'f');
               });
    }

    [[nodiscard]] static bool validSecret(std::string_view value) noexcept {
        return value.size() >= 32 && value.size() <= 128 &&
               std::ranges::all_of(value,
                                   [](unsigned char ch) { return ch >= 0x21 && ch <= 0x7e; });
    }

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
