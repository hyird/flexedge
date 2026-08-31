#pragma once

#include <string>

#include <openssl/crypto.h>

namespace flexedge::node {

class SecretStringGuard final {
  public:
    explicit SecretStringGuard(std::string& value) noexcept : value_(value) {}
    ~SecretStringGuard() { OPENSSL_cleanse(value_.data(), value_.size()); }

    SecretStringGuard(const SecretStringGuard&) = delete;
    SecretStringGuard& operator=(const SecretStringGuard&) = delete;

  private:
    std::string& value_;
};

} // namespace flexedge::node
