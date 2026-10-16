#pragma once

#include <ruvia/web/http_client_types.h>

#include <chrono>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>

namespace service::outbound_http {

// A failure of a background attempt talking to a provider. Permanent when
// retrying the same input cannot succeed; the attempt is then not retried.
class attempt_error : public std::runtime_error {
public:
    attempt_error(std::string_view message, bool permanent)
        : std::runtime_error(std::string(message)),
          permanent_(permanent) {}

    [[nodiscard]] bool permanent() const noexcept {
        return permanent_;
    }

private:
    bool permanent_;
};

// A request that got no reply. Never permanent: the next attempt retries it.
class transport_error final : public attempt_error {
public:
    explicit transport_error(std::string_view message)
        : attempt_error(message, false) {}
};

// The remote service as messages name it ("Cloudflare API", "证书供应商")
// and the end-to-end bound of one request to it.
struct remote_service final {
    std::string_view label_;
    std::chrono::seconds timeout_;
};

// The message of a transport failure: the remote service, then the failure.
[[nodiscard]] inline std::string transport_error_message(const remote_service& remote,
    ruvia::http_client_error::code_type code) {
    using enum ruvia::http_client_error::code_type;
    std::string reason;
    switch (code) {
        case timeout:
            reason = "请求超时（" + std::to_string(remote.timeout_.count()) + " 秒）";
            break;
        case resolve_failed:
            reason = "域名解析失败";
            break;
        case connect_failed:
            reason = "连接失败";
            break;
        case tls_failed:
            reason = "TLS 连接失败";
            break;
        case io_error:
            reason = "网络读写失败";
            break;
        case protocol_unavailable:
        case protocol_error:
            reason = "响应协议错误";
            break;
        case response_too_large:
        case result_budget_exceeded:
            reason = "响应超过大小限制";
            break;
        case queue_full:
            reason = "请求队列繁忙";
            break;
        case cancelled:
        case closing:
            reason = "请求已取消";
            break;
        case not_configured:
            reason = "出站地址未配置";
            break;
        case invalid_request:
            reason = "请求无效";
            break;
    }
    if (reason.empty()) {
        reason = "请求失败";
    }
    const auto ascii = [](char ch) { return (static_cast<unsigned char>(ch) & 0x80U) == 0; };
    // Latin text and CJK text are separated by a space.
    const bool spaced = !remote.label_.empty() && (ascii(remote.label_.back()) || ascii(reason.front()));
    return std::string(remote.label_) + (spaced ? " " : "") + reason;
}

// The stored message and retry decision of a failed attempt.
struct attempt_failure final {
    std::string message_;
    bool permanent_{};
};

// Every background attempt that talks to providers classifies its failure
// here: attempt errors decide themselves, an invalid argument (bad stored
// input) is permanent, anything else is retried.
[[nodiscard]] inline attempt_failure classify_attempt_failure(const std::exception_ptr& exception,
    std::string_view unknown_message) {
    try {
        std::rethrow_exception(exception);
    } catch (const attempt_error& error) {
        return {.message_ = error.what(), .permanent_ = error.permanent()};
    } catch (const std::invalid_argument& error) {
        return {.message_ = error.what(), .permanent_ = true};
    } catch (const std::exception& error) {
        return {.message_ = error.what(), .permanent_ = false};
    } catch (...) {
        return {.message_ = std::string(unknown_message), .permanent_ = false};
    }
}

}  // namespace service::outbound_http
