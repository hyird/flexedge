#pragma once

#include <ruvia/core/operation_options.h>
#include <ruvia/core/task.h>
#include <ruvia/http/http_client.h>
#include <ruvia/http/http_status.h>
#include <ruvia/web/http_client_types.h>

#include <cctype>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "service/features/attempt_trace/outbound_policy.h"
#include "service/features/attempt_trace/recorder.h"
#include "service/utils/sensitive_string.h"

namespace service::outbound_http {

struct header final {
    std::string name_;
    std::string value_;
};

class buffered_response final {
public:
    buffered_response(ruvia::http_status_code status, std::vector<service::outbound_http::header> headers, std::string body)
        : status_(status),
          headers_(std::move(headers)),
          body_(std::move(body)) {}

    [[nodiscard]] ruvia::http_status_code status() const noexcept {
        return status_;
    }
    [[nodiscard]] std::string_view body() const noexcept {
        return body_.view();
    }

    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const noexcept {
        for (const auto& header : headers_) {
            if (equal_ignore_case(header.name_, name)) {
                return header.value_;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] const std::vector<service::outbound_http::header>& headers() const noexcept {
        return headers_;
    }

private:
    [[nodiscard]] static bool equal_ignore_case(std::string_view left,
        std::string_view right) noexcept {
        if (left.size() != right.size()) {
            return false;
        }
        for (std::size_t index = 0; index < left.size(); ++index) {
            const auto left_char = static_cast<unsigned char>(left[index]);
            const auto right_char = static_cast<unsigned char>(right[index]);
            if (std::tolower(left_char) != std::tolower(right_char)) {
                return false;
            }
        }
        return true;
    }

    ruvia::http_status_code status_;
    std::vector<service::outbound_http::header> headers_;
    service::utils::sensitive_string body_;
};

// The single outbound HTTP path of background attempts. Every request and its
// response or transport failure is captured by the attempt's recorder (after
// redaction) before the result is returned or the error rethrown unchanged.
template <typename client_type>
ruvia::task<buffered_response> send_buffered(client_type& client, const service::attempt_trace::outbound_call& call,
    service::attempt_trace::recorder& trace, const ruvia::http_client_request_view& request,
    ruvia::operation_options options = {}) {
    const auto token = trace.begin_http(call, request);
    std::optional<buffered_response> result;
    try {
        auto response = co_await client.with_options(std::move(options)).send(request);
        std::vector<header> headers;
        headers.reserve(response.headers().size());
        for (const auto& header : response.headers()) {
            headers.push_back({std::string(header.name()), std::string(header.value())});
        }
        const auto buffered_body = co_await response.body().read_all();
        const auto bytes = buffered_body.bytes();
        std::string body(bytes.size(), '\0');
        if (!bytes.empty()) {
            std::memcpy(body.data(), bytes.data(), bytes.size());
        }
        result.emplace(response.status(), std::move(headers), std::move(body));
    } catch (const ruvia::http_client_error& error) {
        trace.fail_http(token, error.code());
        throw;
    }
    std::vector<service::attempt_trace::header_entry> captured;
    try {
        captured.reserve(result->headers().size());
        for (const auto& header : result->headers()) {
            captured.push_back({.name_ = header.name_, .value_ = header.value_});
        }
    } catch (...) {
        captured.clear();
    }
    trace.complete_http(token, result->status().value(), captured, result->body());
    co_return std::move(*result);
}

}  // namespace service::outbound_http
