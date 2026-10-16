#pragma once

#include <ruvia/web/context.h>
#include <ruvia/web/error.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include "service/common/types.h"
#include "service/common/uuid.h"

namespace service::common {

// Request query/param/header accessors return std::optional<std::string_view>;
// parse integer inputs strictly at the application boundary.
inline std::optional<std::int64_t> parse_int64(std::optional<std::string_view> input) {
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

inline std::optional<bool> parse_boolean(std::optional<std::string_view> input) {
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

inline std::optional<std::int64_t> parse_entity_tag(std::optional<std::string_view> input) {
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
    return parse_int64(value);
}

inline void set_revision_etag(ruvia::context& c, std::int64_t revision) {
    const auto value = std::string{"\""} + std::to_string(revision) + "\"";
    c.header("ETag", value);
}

inline constexpr std::int64_t unknown_error_code{10000};
inline constexpr std::int64_t validation_error_code{10001};
inline constexpr std::int64_t bad_request_error_code{10002};
inline constexpr std::int64_t not_found_error_code{10003};
inline constexpr std::int64_t server_error_code{10004};
inline constexpr std::int64_t auth_unauthorized_error_code{11004};
inline constexpr std::int64_t auth_session_invalid_error_code{11006};
inline constexpr std::int64_t auth_permission_denied_error_code{11007};

struct app_error_def {
    std::int64_t code_;
    std::string_view message_;
    std::uint16_t status_{400};
};

inline std::int64_t default_business_error_code(std::uint16_t status) {
    switch (status) {
        case 400:
        case 422:
            return bad_request_error_code;
        case 401:
            return auth_unauthorized_error_code;
        case 403:
            return auth_permission_denied_error_code;
        case 404:
            return not_found_error_code;
        case 500:
        case 502:
        case 503:
        case 504:
            return server_error_code;
        default:
            return status >= 500 ? server_error_code : unknown_error_code;
    }
}

inline std::int64_t normalize_business_error_code(std::string_view code, std::uint16_t status) {
    if (!code.empty()) {
        std::int64_t value = 0;
        const auto* first = code.data();
        const auto* last = first + code.size();
        const auto [ptr, ec] = std::from_chars(first, last, value);
        if (ec == std::errc{} && ptr == last) {
            return value;
        }
        if (code == "validation_failed") {
            return validation_error_code;
        }
    }
    return default_business_error_code(status);
}

inline std::string_view response_error_message(ruvia::http_error_info info) noexcept {
    if (!info.validation_issues().empty() && !info.validation_issues().front().message().empty()) {
        return info.validation_issues().front().message();
    }
    if (!info.message().empty()) {
        return info.message();
    }
    return ruvia::http_reason_phrase(info.status());
}

[[noreturn]] inline void throw_app_error(const app_error_def& def) {
    const auto code = std::to_string(def.code_);
    throw ruvia::http_error({
        .status_ = ruvia::http_status_code::from_value(def.status_),
        .code_ = code,
        .message_ = def.message_,
    });
}

[[noreturn]] inline void throw_app_error(std::int64_t code, std::string_view message,
    std::uint16_t status = 400) {
    const auto code_text = std::to_string(code);
    throw ruvia::http_error({
        .status_ = ruvia::http_status_code::from_value(status),
        .code_ = code_text,
        .message_ = message,
    });
}

inline auto require_pagination(ruvia::context& c, std::int64_t default_page_size = 100,
    std::int64_t max_page_size = 1000) {
    const auto parse_positive = [&](std::string_view name, std::int64_t default_value,
                                    std::optional<std::int64_t> maximum = std::nullopt) {
        const auto input = c.req().query(name);
        if (!input) {
            return default_value;
        }
        const auto parsed = parse_int64(input);
        if (!parsed || *parsed <= 0 || (maximum && *parsed > *maximum)) {
            throw_app_error(validation_error_code,
                std::string(name) +
                    (maximum ? " 必须是 1 到 " + std::to_string(*maximum) + " 的整数"
                             : " 必须是正整数"),
                400);
        }
        return *parsed;
    };

    const auto page = parse_positive("page", 1);
    const auto page_size = parse_positive("page_size", default_page_size, max_page_size);
    if (page - 1 > std::numeric_limits<std::int64_t>::max() / page_size) {
        throw_app_error(validation_error_code, "page 超出可查询范围", 400);
    }
    return std::tuple{page, page_size, (page - 1) * page_size};
}

inline std::optional<std::string> require_keyword(std::optional<std::string_view> input,
    std::size_t maximum_bytes = 200) {
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
    if (static_cast<std::size_t>(end - begin) > maximum_bytes) {
        throw_app_error(validation_error_code,
            "keyword 长度不能超过 " + std::to_string(maximum_bytes) + " 字节", 400);
    }
    return std::string(begin, end);
}

inline std::int64_t require_expected_revision(ruvia::context& c) {
    const auto header = c.req().header("If-Match");
    if (!header) {
        throw_app_error(validation_error_code, "更新资源必须提供 If-Match revision ETag", 428);
    }
    const auto revision = parse_entity_tag(header);
    if (!revision || *revision <= 0) {
        throw_app_error(validation_error_code, "If-Match 必须是带引号的正整数 revision ETag", 400);
    }
    return *revision;
}

inline std::string require_uuid_param(ruvia::context& c, std::string_view name) {
    auto value = parse_uuid(c.req().param(name));
    if (!value) {
        throw_app_error(validation_error_code, std::string(name) + " 必须是 UUID", 400);
    }
    return std::move(*value);
}

// `operation_id`: the resource operation, whether or not it needs background work.
inline operation_response operation(ruvia::context& c, std::string_view message,
    const std::optional<std::string>& operation_id = std::nullopt) {
    operation_response response({.resource_ = c.pool()});
    response.set<"code">(0);
    response.set<"message">(message);
    if (operation_id) {
        operation_reference_dto reference({.resource_ = c.pool()});
        reference.set<"operation_id">(*operation_id);
        response.set<"data">(std::move(reference));
    }
    return response;
}

template <typename response_t_type, typename data_t_type>
inline response_t_type ok(auto& c, data_t_type&& data) {
    response_t_type response({.resource_ = c.pool()});
    response.template set<"code">(0);
    response.template set<"message">("ok");
    response.template set<"data">(std::forward<data_t_type>(data));
    return response;
}

inline health_response health(ruvia::context& c) {
    health_data data({.resource_ = c.pool()});
    data.set<"status">("ok");
    health_response response({.resource_ = c.pool()});
    response.set<"code">(0);
    response.set<"message">("ok");
    response.set<"data">(std::move(data));
    return response;
}

inline count_response count(ruvia::context& c, std::int64_t created_count, std::string_view message) {
    count_data data({.resource_ = c.pool()});
    data.set<"created_count">(static_cast<ruvia::int64>(created_count));
    count_response response({.resource_ = c.pool()});
    response.set<"code">(0);
    response.set<"message">(message);
    response.set<"data">(std::move(data));
    return response;
}

inline error_response error(ruvia::context& c, std::int64_t code, std::string_view message) {
    error_response response({.resource_ = c.pool()});
    response.set<"code">(code);
    response.set<"message">(message);
    return response;
}

}  // namespace service::common
