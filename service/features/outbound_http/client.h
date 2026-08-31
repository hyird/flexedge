#pragma once

#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ruvia/core/Task.h>
#include <ruvia/http/HttpClient.h>
#include <ruvia/http/HttpStatus.h>
#include <ruvia/core/OperationOptions.h>

#include "service/utils/sensitive_string.h"

namespace service::outbound_http {

struct Header final {
    std::string name;
    std::string value;
};

class BufferedResponse final {
  public:
    BufferedResponse(ruvia::HttpStatusCode status, std::vector<Header> headers, std::string body)
        : status_(status), headers_(std::move(headers)), body_(std::move(body)) {}

    [[nodiscard]] ruvia::HttpStatusCode status() const noexcept { return status_; }
    [[nodiscard]] std::string_view body() const noexcept { return body_.view(); }

    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const noexcept {
        for (const auto& header : headers_) {
            if (equalIgnoreCase(header.name, name)) {
                return header.value;
            }
        }
        return std::nullopt;
    }

  private:
    [[nodiscard]] static bool equalIgnoreCase(std::string_view left,
                                              std::string_view right) noexcept {
        if (left.size() != right.size()) {
            return false;
        }
        for (std::size_t index = 0; index < left.size(); ++index) {
            const auto leftChar = static_cast<unsigned char>(left[index]);
            const auto rightChar = static_cast<unsigned char>(right[index]);
            if (std::tolower(leftChar) != std::tolower(rightChar)) {
                return false;
            }
        }
        return true;
    }

    ruvia::HttpStatusCode status_;
    std::vector<Header> headers_;
    service::utils::SensitiveString body_;
};

template <typename Client>
ruvia::Task<BufferedResponse> sendBuffered(Client& client,
                                           const ruvia::HttpClientRequestView& request,
                                           ruvia::OperationOptions options = {}) {
    auto response = co_await client.withOptions(std::move(options)).send(request);
    std::vector<Header> headers;
    headers.reserve(response.headers().size());
    for (const auto& header : response.headers()) {
        headers.push_back({std::string(header.name()), std::string(header.value())});
    }
    const auto bufferedBody = co_await response.body().readAll();
    co_return BufferedResponse(response.status(), std::move(headers),
                               std::string(bufferedBody.data(), bufferedBody.size()));
}

} // namespace service::outbound_http
