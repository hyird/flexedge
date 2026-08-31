#pragma once

#include <string>
#include <string_view>
#include <utility>

#include <openssl/crypto.h>

namespace service::utils {

template <typename Buffer> class SensitiveBufferGuard final {
  public:
    explicit SensitiveBufferGuard(Buffer& value) noexcept : value_(value) {}

    ~SensitiveBufferGuard() {
        if (!value_.empty()) {
            OPENSSL_cleanse(value_.data(), value_.size() * sizeof(typename Buffer::value_type));
        }
    }

    SensitiveBufferGuard(const SensitiveBufferGuard&) = delete;
    SensitiveBufferGuard& operator=(const SensitiveBufferGuard&) = delete;
    SensitiveBufferGuard(SensitiveBufferGuard&&) = delete;
    SensitiveBufferGuard& operator=(SensitiveBufferGuard&&) = delete;

  private:
    Buffer& value_;
};

class SensitiveString final {
  public:
    SensitiveString() = default;

    explicit SensitiveString(std::string&& value) : value_(std::move(value)) { cleanse(value); }

    ~SensitiveString() { cleanse(value_); }

    SensitiveString(const SensitiveString&) = delete;
    SensitiveString& operator=(const SensitiveString&) = delete;
    SensitiveString(SensitiveString&& other) noexcept : value_(std::move(other.value_)) {
        cleanse(other.value_);
    }

    SensitiveString& operator=(SensitiveString&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        cleanse(value_);
        value_ = std::move(other.value_);
        cleanse(other.value_);
        return *this;
    }

    [[nodiscard]] std::string_view view() const noexcept { return value_; }

  private:
    static void cleanse(std::string& value) noexcept {
        if (!value.empty()) {
            OPENSSL_cleanse(value.data(), value.size());
        }
        value.clear();
    }

    std::string value_;
};

} // namespace service::utils
