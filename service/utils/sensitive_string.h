#pragma once

#include <string>
#include <string_view>
#include <utility>

#include <openssl/crypto.h>

namespace service::utils {

template <typename buffer_type>
class sensitive_buffer_guard final {
public:
    explicit sensitive_buffer_guard(buffer_type& value) noexcept
        : value_(value) {}

    ~sensitive_buffer_guard() {
        if (!value_.empty()) {
            OPENSSL_cleanse(value_.data(), value_.size() * sizeof(typename buffer_type::value_type));
        }
    }

    sensitive_buffer_guard(const sensitive_buffer_guard&) = delete;
    sensitive_buffer_guard& operator=(const sensitive_buffer_guard&) = delete;
    sensitive_buffer_guard(sensitive_buffer_guard&&) = delete;
    sensitive_buffer_guard& operator=(sensitive_buffer_guard&&) = delete;

private:
    buffer_type& value_;
};

class sensitive_string final {
public:
    sensitive_string() = default;

    explicit sensitive_string(std::string&& value)
        : value_(std::move(value)) {
        cleanse(value);
    }

    ~sensitive_string() {
        cleanse(value_);
    }

    sensitive_string(const sensitive_string&) = delete;
    sensitive_string& operator=(const sensitive_string&) = delete;
    sensitive_string(sensitive_string&& other) noexcept
        : value_(std::move(other.value_)) {
        cleanse(other.value_);
    }

    sensitive_string& operator=(sensitive_string&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        cleanse(value_);
        value_ = std::move(other.value_);
        cleanse(other.value_);
        return *this;
    }

    [[nodiscard]] std::string_view view() const noexcept {
        return value_;
    }

private:
    static void cleanse(std::string& value) noexcept {
        if (!value.empty()) {
            OPENSSL_cleanse(value.data(), value.size());
        }
        value.clear();
    }

    std::string value_;
};

}  // namespace service::utils
