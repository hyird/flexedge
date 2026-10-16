#pragma once

#include <ruvia/core/operation_options.h>
#include <ruvia/core/task.h>
#include <ruvia/http/http_client.h>
#include <ruvia/http/http_status.h>
#include <ruvia/web/http_client_types.h>

#include <chrono>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "service/features/attempt_trace/outbound_policy.h"
#include "service/features/attempt_trace/recorder.h"
#include "service/features/outbound_http/error.h"
#include "service/features/outbound_http/quota.h"
#include "service/utils/sensitive_string.h"
#include "service/utils/text.h"

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
            if (service::utils::equals_ignore_case(header.name_, name)) {
                return header.value_;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] const std::vector<service::outbound_http::header>& headers() const noexcept {
        return headers_;
    }

private:
    ruvia::http_status_code status_;
    std::vector<service::outbound_http::header> headers_;
    service::utils::sensitive_string body_;
};

// The single outbound HTTP path of background attempts. Every request and its
// response or transport failure is captured by the attempt's recorder (after
// redaction) before the result is returned; a transport failure is rethrown
// as a transport_error naming `remote`. The request is bounded by the remote
// service's timeout and the context's stop token.
template <typename runtime_type>
ruvia::task<buffered_response> send_buffered(runtime_type& context, const quota_scope& scope,
    const service::attempt_trace::outbound_call& call, const remote_service& remote,
    service::attempt_trace::recorder& trace, const ruvia::http_client_request_view& request) {
    co_await acquire_quota(context, call.origin_alias_, scope);
    auto&& client = context.get_http_client(call.origin_alias_);
    const auto token = trace.begin_http(call, request);
    std::optional<buffered_response> result;
    try {
        auto response = co_await client
                            .with_options({.timeout_ = std::chrono::duration_cast<std::chrono::milliseconds>(
                                               remote.timeout_),
                                .stop_token_ = context.get_stop_token()})
                            .send(request);
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
        throw transport_error(transport_error_message(remote, error.code()));
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
