#pragma once

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include <ruvia/web/Context.h>
#include <ruvia/web/Error.h>

#include "service/common/types.h"

namespace service::common {

// Request query/param/header accessors return std::optional<std::string_view>;
// parse integer inputs strictly at the application boundary.
inline std::optional<std::int64_t> parseInt64(std::optional<std::string_view> input) {
    if (!input || input->empty()) {
        return std::nullopt;
    }
    std::int64_t value = 0;
    const auto* first = input->data();
    const auto* last = first + input->size();
    const auto [ptr, ec] = std::from_chars(first, last, value);
    if (ec == std::errc{} && ptr == last) {
        return value;
    }
    return std::nullopt;
}

inline std::optional<bool> parseBoolean(std::optional<std::string_view> input) {
    if (!input) {
        return std::nullopt;
    }
    if (*input == "true") {
        return true;
    }
    if (*input == "false") {
        return false;
    }
    return std::nullopt;
}

inline std::optional<std::int64_t> parseEntityTag(std::optional<std::string_view> input) {
    if (!input) {
        return std::nullopt;
    }
    auto value = *input;
    if (value.starts_with("W/")) {
        return std::nullopt;
    }
    if (value.size() < 3 || value.front() != '"' || value.back() != '"') {
        return std::nullopt;
    }
    value.remove_prefix(1);
    value.remove_suffix(1);
    return parseInt64(value);
}

inline void setRevisionEtag(ruvia::Context& c, std::int64_t revision) {
    const auto value = std::string{"\""} + std::to_string(revision) + "\"";
    c.header("ETag", value);
}

inline std::optional<std::string> parseUuid(std::optional<std::string_view> input) {
    if (!input || input->size() != 36) {
        return std::nullopt;
    }

    std::string value;
    value.reserve(input->size());
    for (std::size_t index = 0; index < input->size(); ++index) {
        const char ch = (*input)[index];
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (ch != '-') {
                return std::nullopt;
            }
            value.push_back(ch);
            continue;
        }
        if (!std::isxdigit(static_cast<unsigned char>(ch))) {
            return std::nullopt;
        }
        value.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return value;
}

inline constexpr std::int64_t kUnknownErrorCode{10000};
inline constexpr std::int64_t kValidationErrorCode{10001};
inline constexpr std::int64_t kBadRequestErrorCode{10002};
inline constexpr std::int64_t kNotFoundErrorCode{10003};
inline constexpr std::int64_t kServerErrorCode{10004};
inline constexpr std::int64_t kAuthUnauthorizedErrorCode{11004};
inline constexpr std::int64_t kAuthSessionInvalidErrorCode{11006};
inline constexpr std::int64_t kAuthPermissionDeniedErrorCode{11007};

struct AppErrorDef {
    std::int64_t code;
    std::string_view message;
    std::uint16_t status{400};
};

inline std::int64_t defaultBusinessErrorCode(std::uint16_t status) {
    switch (status) {
    case 400:
    case 422:
        return kBadRequestErrorCode;
    case 401:
        return kAuthUnauthorizedErrorCode;
    case 403:
        return kAuthPermissionDeniedErrorCode;
    case 404:
        return kNotFoundErrorCode;
    case 500:
    case 502:
    case 503:
    case 504:
        return kServerErrorCode;
    default:
        return status >= 500 ? kServerErrorCode : kUnknownErrorCode;
    }
}

inline std::int64_t normalizeBusinessErrorCode(std::string_view code, std::uint16_t status) {
    if (!code.empty()) {
        std::int64_t value = 0;
        const auto* first = code.data();
        const auto* last = first + code.size();
        const auto [ptr, ec] = std::from_chars(first, last, value);
        if (ec == std::errc{} && ptr == last) {
            return value;
        }
        if (code == "validation_failed") {
            return kValidationErrorCode;
        }
    }
    return defaultBusinessErrorCode(status);
}

inline std::string_view responseErrorMessage(ruvia::HttpErrorInfo info) noexcept {
    if (!info.validationIssues().empty() && !info.validationIssues().front().message().empty()) {
        return info.validationIssues().front().message();
    }
    if (!info.message().empty()) {
        return info.message();
    }
    return ruvia::httpReasonPhrase(info.status());
}

[[noreturn]] inline void throwAppError(const AppErrorDef& def) {
    const auto code = std::to_string(def.code);
    throw ruvia::HttpError({
        .status = ruvia::HttpStatusCode::fromValue(def.status),
        .code = code,
        .message = def.message,
    });
}

[[noreturn]] inline void throwAppError(std::int64_t code, std::string_view message,
                                       std::uint16_t status = 400) {
    const auto codeText = std::to_string(code);
    throw ruvia::HttpError({
        .status = ruvia::HttpStatusCode::fromValue(status),
        .code = codeText,
        .message = message,
    });
}

inline auto requirePagination(ruvia::Context& c, std::int64_t defaultPageSize = 10,
                              std::int64_t maxPageSize = 100) {
    const auto parsePositive = [&](std::string_view name, std::int64_t defaultValue,
                                   std::optional<std::int64_t> maximum = std::nullopt) {
        const auto input = c.req().query(name);
        if (!input) {
            return defaultValue;
        }
        const auto parsed = parseInt64(input);
        if (!parsed || *parsed <= 0 || (maximum && *parsed > *maximum)) {
            throwAppError(kValidationErrorCode,
                          std::string(name) +
                              (maximum ? " 必须是 1 到 " + std::to_string(*maximum) + " 的整数"
                                       : " 必须是正整数"),
                          400);
        }
        return *parsed;
    };

    const auto page = parsePositive("page", 1);
    const auto pageSize = parsePositive("page_size", defaultPageSize, maxPageSize);
    if (page - 1 > std::numeric_limits<std::int64_t>::max() / pageSize) {
        throwAppError(kValidationErrorCode, "page 超出可查询范围", 400);
    }
    return std::tuple{page, pageSize, (page - 1) * pageSize};
}

inline std::optional<std::string> requireKeyword(std::optional<std::string_view> input,
                                                 std::size_t maximumBytes = 200) {
    if (!input) {
        return std::nullopt;
    }
    const auto begin = std::find_if_not(input->begin(), input->end(),
                                        [](unsigned char ch) { return std::isspace(ch) != 0; });
    const auto end = std::find_if_not(input->rbegin(), input->rend(), [](unsigned char ch) {
                         return std::isspace(ch) != 0;
                     }).base();
    if (begin >= end) {
        return std::nullopt;
    }
    if (static_cast<std::size_t>(end - begin) > maximumBytes) {
        throwAppError(kValidationErrorCode,
                      "keyword 长度不能超过 " + std::to_string(maximumBytes) + " 字节", 400);
    }
    return std::string(begin, end);
}

inline std::int64_t requireExpectedRevision(ruvia::Context& c) {
    const auto header = c.req().header("If-Match");
    if (!header) {
        throwAppError(kValidationErrorCode, "更新资源必须提供 If-Match revision ETag", 428);
    }
    const auto revision = parseEntityTag(header);
    if (!revision || *revision <= 0) {
        throwAppError(kValidationErrorCode, "If-Match 必须是带引号的正整数 revision ETag", 400);
    }
    return *revision;
}

inline std::string requireUuidParam(ruvia::Context& c, std::string_view name) {
    auto value = parseUuid(c.req().param(name));
    if (!value) {
        throwAppError(kValidationErrorCode, std::string(name) + " 必须是 UUID", 400);
    }
    return std::move(*value);
}

inline OperationResponse operation(ruvia::Context& c, std::string_view message) {
    OperationResponse response(c);
    response.set<"code">(0);
    response.set<"message">(message);
    return response;
}

template <typename ResponseT, typename DataT> inline ResponseT ok(ruvia::Context& c, DataT&& data) {
    ResponseT response(c);
    response.template set<"code">(0);
    response.template set<"message">("ok");
    response.template set<"data">(std::forward<DataT>(data));
    return response;
}

inline HealthResponse health(ruvia::Context& c) {
    HealthData data(c);
    data.set<"status">("ok");
    HealthResponse response(c);
    response.set<"code">(0);
    response.set<"message">("ok");
    response.set<"data">(std::move(data));
    return response;
}

inline CountResponse count(ruvia::Context& c, std::int64_t createdCount, std::string_view message) {
    CountData data(c);
    data.set<"createdCount">(static_cast<ruvia::Int64>(createdCount));
    CountResponse response(c);
    response.set<"code">(0);
    response.set<"message">(message);
    response.set<"data">(std::move(data));
    return response;
}

inline ErrorResponse error(ruvia::Context& c, std::int64_t code, std::string_view message) {
    ErrorResponse response(c);
    response.set<"code">(code);
    response.set<"message">(message);
    return response;
}

} // namespace service::common
