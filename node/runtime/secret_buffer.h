#pragma once

#include <string>

#include <openssl/crypto.h>

namespace flexedge::node {

class secret_string_guard final {
public:
    explicit secret_string_guard(std::string& value) noexcept
        : value_(value) {}
    ~secret_string_guard() {
        OPENSSL_cleanse(value_.data(), value_.size());
    }

    secret_string_guard(const secret_string_guard&) = delete;
    secret_string_guard& operator=(const secret_string_guard&) = delete;

private:
    std::string& value_;
};

}  // namespace flexedge::node
