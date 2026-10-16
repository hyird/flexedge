#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "node/data/http1_request_reader.h"
#include "node/proto/edge_control.pb.h"
#include "node/runtime/log_buffer.h"

namespace flexedge::node {

// Collects one request's access log record under its website's access log policy and
// records it at most once, when the response status is known.
class access_log_capture final {
public:
    access_log_capture(node_log_buffer::producer& logs, bool secure, std::string tls_fingerprint)
        : logs_(logs),
          tls_fingerprint_(std::move(tls_fingerprint)),
          secure_(secure) {}

    void request(const http1_request_reader& parsed, std::string_view host) {
        method_.assign(parsed.request().method());
        target_.assign(parsed.request().target());
        host_.assign(host);
    }

    void website(const http1_request_reader& parsed, const v2::Website& website) {
        website_id_ = website.id();
        enabled_ = website.access_log_enabled();
        if (!enabled_) {
            return;
        }
        response_headers_enabled_ = website.access_log_response_headers();
        client_abort_enabled_ = website.access_log_client_abort();
        apply_status_code_ranges(website);
        capture_request(parsed, website);
    }

    [[nodiscard]] bool client_abort_enabled() const noexcept {
        return client_abort_enabled_;
    }

    // `wire` starts with the HTTP/1.1 response head sent to the client.
    void response_head(std::string_view wire) {
        if (!response_headers_enabled_ || !response_headers_.empty()) {
            return;
        }
        response_headers_ = header_block(response_fields(wire), node_log_buffer::max_response_headers_bytes);
    }

    void record(std::uint32_t status_code, std::uint64_t response_bytes,
        std::string_view client_ip) noexcept {
        if (recorded_ || !enabled_ || website_id_.empty() || !status_code_enabled(status_code)) {
            return;
        }
        recorded_ = true;
        const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_at_);
        try {
            logs_.access({
                .website_id_ = website_id_,
                .client_ip_ = std::string(client_ip),
                .protocol_ = secure_ ? "https" : "http",
                .method_ = method_,
                .host_ = host_,
                .target_ = target_,
                .status_code_ = status_code,
                .request_bytes_ = request_body_bytes_,
                .response_bytes_ = response_bytes,
                .duration_ms_ = duration.count() < 0 ? std::uint64_t{}
                                                     : static_cast<std::uint64_t>(duration.count()),
                .user_agent_ = user_agent_,
                .referer_ = referer_,
                .request_headers_ = request_headers_,
                .request_body_ = request_body_,
                .request_body_truncated_ = request_body_truncated_,
                .tls_fingerprint_ = tls_fingerprint_,
                .response_headers_ = response_headers_,
                .query_string_ = query_string_,
                .cookies_ = cookies_,
            });
        } catch (...) {
            // A record that cannot be queued is dropped like one rejected by a full buffer.
        }
    }

private:
    static void append_bounded(std::string& result, std::string_view value, std::size_t maximum) {
        if (result.size() < maximum) {
            result.append(value.substr(0, maximum - result.size()));
        }
    }

    static std::string request_header_block(const http1_request_reader& parsed) {
        constexpr auto maximum = node_log_buffer::max_request_headers_bytes;
        std::string result;
        for (const auto& header : parsed.request().headers()) {
            append_bounded(result, header.name(), maximum);
            append_bounded(result, ": ", maximum);
            append_bounded(result, header.value(), maximum);
            append_bounded(result, "\n", maximum);
            if (result.size() >= maximum) {
                break;
            }
        }
        return result;
    }

    // Field lines of an HTTP/1.1 head, without the status line.
    static std::string_view response_fields(std::string_view wire) noexcept {
        const auto head_end = wire.find("\r\n\r\n");
        const auto status_line_end = wire.find("\r\n");
        if (head_end == std::string_view::npos || status_line_end + 2 > head_end) {
            return {};
        }
        return wire.substr(status_line_end + 2, head_end - status_line_end - 2);
    }

    static std::string header_block(std::string_view block, std::size_t maximum) {
        std::string result;
        while (!block.empty() && result.size() < maximum) {
            const auto line_end = block.find("\r\n");
            const auto line = line_end == std::string_view::npos ? block : block.substr(0, line_end);
            const auto colon = line.find(':');
            if (colon == std::string_view::npos) {
                append_bounded(result, line, maximum);
            } else {
                auto value = line.substr(colon + 1);
                while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
                    value.remove_prefix(1);
                }
                append_bounded(result, line.substr(0, colon), maximum);
                append_bounded(result, ": ", maximum);
                append_bounded(result, value, maximum);
            }
            append_bounded(result, "\n", maximum);
            if (line_end == std::string_view::npos) {
                break;
            }
            block.remove_prefix(line_end + 2);
        }
        return result;
    }

    static std::string query_string(std::string_view target) {
        const auto question = target.find('?');
        if (question == std::string_view::npos) {
            return {};
        }
        const auto fragment = target.find('#', question + 1);
        return std::string(target.substr(question + 1, fragment == std::string_view::npos
                                                           ? std::string_view::npos
                                                           : fragment - question - 1));
    }

    void apply_status_code_ranges(const v2::Website& website) {
        if (website.access_log_status_code_ranges().empty()) {
            status_code_ranges_.fill(true);
            return;
        }
        status_code_ranges_.fill(false);
        for (const auto& value : website.access_log_status_code_ranges()) {
            if (value.size() == 3 && value[0] >= '1' && value[0] <= '5' && value.ends_with("xx")) {
                status_code_ranges_[static_cast<std::size_t>(value[0] - '1')] = true;
            }
        }
    }

    void capture_request(const http1_request_reader& parsed, const v2::Website& website) {
        const auto body = parsed.body();
        request_body_bytes_ = body ? static_cast<std::uint64_t>(body->size()) : 0;
        if (website.access_log_query_params()) {
            query_string_ = query_string(parsed.request().target());
        }
        if (website.access_log_cookies()) {
            if (const auto cookies = parsed.request().header("Cookie")) {
                cookies_.assign(*cookies);
            }
        }
        if (website.access_log_referer()) {
            if (const auto referer = parsed.request().header("Referer")) {
                referer_.assign(*referer);
            }
        }
        if (website.access_log_user_agent()) {
            if (const auto user_agent = parsed.request().header("User-Agent")) {
                user_agent_.assign(*user_agent);
            }
        }
        if (website.access_log_request_headers()) {
            request_headers_ = request_header_block(parsed);
        }
        if (website.access_log_request_body() && parsed.has_body() && body) {
            request_body_truncated_ = body->size() > node_log_buffer::max_request_body_bytes;
            request_body_.assign(body->substr(0, node_log_buffer::max_request_body_bytes));
        }
    }

    [[nodiscard]] bool status_code_enabled(std::uint32_t status_code) const noexcept {
        if (status_code == 499) {
            return client_abort_enabled_;
        }
        const auto range = status_code / 100;
        return range >= 1 && range <= 5 && status_code_ranges_[range - 1];
    }

    node_log_buffer::producer& logs_;
    std::chrono::steady_clock::time_point started_at_{std::chrono::steady_clock::now()};
    std::string website_id_;
    std::string method_;
    std::string target_;
    std::string host_;
    std::uint64_t request_body_bytes_{};
    std::string user_agent_;
    std::string referer_;
    std::string request_headers_;
    std::string request_body_;
    std::string response_headers_;
    std::string query_string_;
    std::string cookies_;
    std::string tls_fingerprint_;
    std::array<bool, 5> status_code_ranges_{true, true, true, true, true};
    bool secure_{};
    bool enabled_{};
    bool response_headers_enabled_{};
    bool client_abort_enabled_{};
    bool request_body_truncated_{};
    bool recorded_{};
};

}  // namespace flexedge::node
