#pragma once

#include <ruvia/http/http1_client_exchange_state.h>
#include <ruvia/http/http1_client_response_parser.h>
#include <ruvia/http/http1_close_policy.h>
#include <ruvia/http/http_client_response_head.h>
#include <ruvia/http/http_status.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include "node/data/chunked_wire_tracker.h"
#include "node/data/origin_request_codec.h"

namespace flexedge::node {

enum class origin_response_head_status { need_more,
    ready,
    failure };

class origin_response_codec final {
public:
    explicit origin_response_codec(ruvia::http1_client_exchange_state response_exchange, bool hsts_enabled,
        const v2::RouteRule* route = nullptr)
        : parser_(std::make_unique<ruvia::http1_client_response_parser>(std::move(response_exchange))),
          hsts_enabled_(hsts_enabled),
          route_(route ? std::optional<v2::RouteRule>{*route} : std::nullopt) {}

    [[nodiscard]] origin_response_head_status consume_head(std::string_view bytes) {
        head_bytes_.append(bytes);
        for (;;) {
            auto result = parser_->parse(std::string_view(head_bytes_).substr(head_offset_));
            if (result.need_more()) {
                return head_bytes_.size() >= max_head_bytes ? origin_response_head_status::failure
                                                            : origin_response_head_status::need_more;
            }
            const auto* parsed = result.parsed();
            if (parsed == nullptr) {
                return origin_response_head_status::failure;
            }
            const auto head_start = head_offset_;
            head_offset_ += parsed->consumed_bytes();
            if (parsed->plan().informational()) {
                if (head_offset_ == head_bytes_.size()) {
                    return origin_response_head_status::need_more;
                }
                continue;
            }
            tunnel_ = parsed->plan().protocol_upgrade() != nullptr;
            const auto body = std::string_view(head_bytes_).substr(head_offset_);
            if (!configure_body(parsed->plan(), body)) {
                return origin_response_head_status::failure;
            }
            status_code_ = static_cast<std::uint16_t>(parsed->head().status().value());
            output_.assign(head_bytes_, 0, head_start);
            append_head(parsed->head());
            if (tunnel_ || body_kind_ != body_kind::none) {
                output_.append(body);
            }
            return origin_response_head_status::ready;
        }
    }

    [[nodiscard]] bool consume_body(std::string_view bytes) {
        if (complete_) {
            return bytes.empty();
        }
        if (body_kind_ == body_kind::known_length) {
            if (bytes.size() > remaining_) {
                return false;
            }
            remaining_ -= bytes.size();
            complete_ = remaining_ == 0;
            return true;
        }
        if (body_kind_ == body_kind::chunked) {
            const auto status = chunked_.consume(bytes);
            complete_ = status == chunked_wire_status::complete;
            return status != chunked_wire_status::invalid;
        }
        return body_kind_ == body_kind::close_delimited;
    }

    [[nodiscard]] std::string take_output() {
        return std::move(output_);
    }
    [[nodiscard]] bool complete() const noexcept {
        return complete_;
    }
    [[nodiscard]] bool chunked() const noexcept {
        return body_kind_ == body_kind::chunked;
    }
    [[nodiscard]] bool tunnel() const noexcept {
        return tunnel_;
    }
    [[nodiscard]] bool origin_reusable() const noexcept {
        return complete_ && !tunnel_ && origin_reusable_;
    }
    [[nodiscard]] bool eof_completes_response() const noexcept {
        return body_kind_ == body_kind::close_delimited;
    }
    [[nodiscard]] std::optional<std::uint16_t> status_code() const noexcept {
        return status_code_;
    }

private:
    static constexpr std::size_t max_head_bytes = 1024 * 1024;
    enum class body_kind { none,
        known_length,
        chunked,
        close_delimited,
        tunnel };

    static bool response_hop_by_hop(std::string_view name) noexcept {
        return http_header_name(name, "Connection") || http_header_name(name, "Keep-Alive") ||
               http_header_name(name, "Proxy-Connection") || http_header_name(name, "Upgrade");
    }

    static std::vector<std::string> connection_tokens(const ruvia::http_client_response_head& head) {
        std::vector<std::string> connection_tokens;
        for (const auto& header : head.headers()) {
            if (!http_header_name(header.name(), "Connection")) {
                continue;
            }
            auto value = header.value();
            while (!value.empty()) {
                const auto comma = value.find(',');
                auto token = value.substr(0, comma);
                while (!token.empty() && (token.front() == ' ' || token.front() == '\t')) {
                    token.remove_prefix(1);
                }
                while (!token.empty() && (token.back() == ' ' || token.back() == '\t')) {
                    token.remove_suffix(1);
                }
                if (!token.empty()) {
                    connection_tokens.emplace_back(token);
                }
                if (comma == std::string_view::npos) {
                    break;
                }
                value.remove_prefix(comma + 1);
            }
        }
        return connection_tokens;
    }

    void append_status_line(const ruvia::http_client_response_head& head) {
        output_.append("HTTP/1.1 ");
        output_.append(std::to_string(head.status().value()));
        const auto reason = ruvia::http_reason_phrase(head.status());
        if (!reason.empty()) {
            output_.push_back(' ');
            output_.append(reason);
        }
        output_.append("\r\n");
    }

    void append_response_headers(const ruvia::http_client_response_head& head,
        const std::vector<std::string>& connection_tokens) {
        for (const auto& header : head.headers()) {
            const auto preserve_upgrade = tunnel_ && (http_header_name(header.name(), "Connection") ||
                                                         http_header_name(header.name(), "Upgrade"));
            const auto named_by_connection =
                std::ranges::any_of(connection_tokens, [&](const auto& token) {
                    return http_header_name(header.name(), token);
                });
            if ((!preserve_upgrade && (response_hop_by_hop(header.name()) || named_by_connection)) ||
                (hsts_enabled_ && http_header_name(header.name(), "Strict-Transport-Security")) ||
                (route_.has_value() &&
                    std::ranges::any_of(route_->response_headers(), [&](const auto& mutation) {
                        return http_header_name(header.name(), mutation.name());
                    }))) {
                continue;
            }
            output_.append(header.name());
            output_.append(": ");
            output_.append(header.value());
            output_.append("\r\n");
        }
        if (route_) {
            for (const auto& mutation : route_->response_headers()) {
                output_.append(mutation.name());
                output_.append(": ");
                output_.append(mutation.value());
                output_.append("\r\n");
            }
        }
    }

    void append_proxy_headers() {
        if (!tunnel_) {
            output_.append("Connection: close\r\n");
        }
        if (hsts_enabled_ && !tunnel_) {
            output_.append("Strict-Transport-Security: max-age=31536000\r\n");
        }
        output_.append("\r\n");
    }

    void append_head(const ruvia::http_client_response_head& head) {
        const auto connection_token_values = connection_tokens(head);
        append_status_line(head);
        append_response_headers(head, connection_token_values);
        append_proxy_headers();
    }

    bool configure_body(const ruvia::http1_client_response_plan& plan, std::string_view initial_body) {
        origin_reusable_ = plan_allows_reuse(plan);
        if (tunnel_) {
            body_kind_ = body_kind::tunnel;
            return true;
        }
        if (const auto* known = plan.known_length()) {
            body_kind_ = body_kind::known_length;
            remaining_ = known->content_length();
            return consume_body(initial_body);
        }
        if (plan.chunked()) {
            body_kind_ = body_kind::chunked;
            return consume_body(initial_body);
        }
        if (plan.close_delimited()) {
            body_kind_ = body_kind::close_delimited;
            return true;
        }
        if (const auto* zero = plan.zero_content()) {
            if (const auto* known = zero->known_length()) {
                body_kind_ = body_kind::known_length;
                remaining_ = known->content_length();
                return consume_body(initial_body);
            }
            if (zero->chunked()) {
                body_kind_ = body_kind::chunked;
                return consume_body(initial_body);
            }
            body_kind_ = body_kind::close_delimited;
            return true;
        }
        body_kind_ = body_kind::none;
        complete_ = true;
        return initial_body.empty();
    }

    static bool plan_allows_reuse(const ruvia::http1_client_response_plan& plan) noexcept {
        const auto allows_reuse = [](const auto* response) {
            return response->persistence() == ruvia::http1_close_policy::allow_reuse;
        };
        if (const auto* response = plan.without_content()) {
            return allows_reuse(response);
        }
        if (const auto* response = plan.known_length()) {
            return allows_reuse(response);
        }
        if (const auto* response = plan.chunked()) {
            return allows_reuse(response);
        }
        if (const auto* response = plan.zero_content()) {
            if (const auto* known = response->known_length()) {
                return allows_reuse(known);
            }
            if (const auto* chunked = response->chunked()) {
                return allows_reuse(chunked);
            }
        }
        return false;
    }

    std::unique_ptr<ruvia::http1_client_response_parser> parser_;
    std::string head_bytes_;
    std::size_t head_offset_{};
    std::string output_;
    body_kind body_kind_{body_kind::none};
    std::size_t remaining_{};
    chunked_wire_tracker chunked_;
    std::optional<std::uint16_t> status_code_;
    bool hsts_enabled_{};
    // A resolved phase policy belongs to a request, not the configuration snapshot.
    // Retain it while asynchronous origin response parsing is still in progress.
    std::optional<v2::RouteRule> route_;
    bool tunnel_{};
    bool complete_{};
    bool origin_reusable_{};
};

}  // namespace flexedge::node
