#pragma once

#include <ruvia/http/http1_chunked_body_decoder.h>
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
#include <utility>
#include <vector>

#include "common/route_policy.h"
#include "node/data/buffered_proxy_response.h"
#include "node/data/origin_request_codec.h"
#include "node/data/response_cache.h"

namespace flexedge::node {

enum class origin_response_head_status { need_more,
    ready,
    failure,
    exhausted };

enum class origin_response_body_status { accepted,
    failure,
    exhausted };

// Parses one origin HTTP/1 response with Ruvia and applies the proxy response policy in
// one place: hop-by-hop fields (RFC 9110 section 7.6.1) and fields named by Connection
// are dropped, and HSTS and route response headers replace any origin value.
//
// A streaming codec emits the rewritten head through take_output(); the caller relays
// every later body byte verbatim, so Transfer-Encoding and Content-Length pass through
// and the codec only tracks where the message ends. A buffering codec decodes the body
// into take_response() for response compression, and frame() re-frames it with
// Content-Length, dropping the transfer coding and its trailers.
class origin_response_codec final {
public:
    // `route` belongs to the request and must outlive the codec.
    origin_response_codec(ruvia::http1_client_exchange_state response_exchange, bool hsts_enabled,
        const evaluated_route_policy* route)
        : parser_(std::make_unique<ruvia::http1_client_response_parser>(std::move(response_exchange))),
          hsts_enabled_(hsts_enabled),
          route_(route) {}

    // Buffers the response; every retained head and body byte is charged to `reservation`.
    origin_response_codec(ruvia::http1_client_exchange_state response_exchange, bool hsts_enabled,
        const evaluated_route_policy* route, buffered_bytes_lease reservation)
        : origin_response_codec(std::move(response_exchange), hsts_enabled, route) {
        buffered_.emplace();
        buffered_->reservation_ = std::move(reservation);
    }

    void capture_for_cache(std::size_t limit, buffered_bytes_lease reservation) {
        captured_.emplace();
        captured_->reservation_ = std::move(reservation);
        capture_limit_ = limit;
    }
    [[nodiscard]] std::optional<buffered_proxy_response> take_capture() {
        auto result = std::move(captured_);
        captured_.reset();
        return result;
    }
    [[nodiscard]] std::size_t buffered_body_size() const {
        return buffered_ ? buffered_->body_.size() : 0;
    }
    void set_browser_cache_ttl(std::int64_t seconds, const std::vector<std::int64_t>& statuses) {
        browser_cache_ttl_ = seconds;
        browser_cache_statuses_ = statuses;
    }
    void set_cache_status(std::string value) {
        cache_status_ = std::move(value);
    }

    [[nodiscard]] origin_response_head_status consume_head(std::string_view bytes) {
        if (buffered_ && !buffered_->reservation_.try_grow(bytes.size())) {
            return origin_response_head_status::exhausted;
        }
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
            status_code_ = static_cast<std::uint16_t>(parsed->head().status().value());
            configure_body(parsed->plan());
            if (browser_cache_ttl_ > 0) {
                cache_headers headers;
                for (const auto& header : parsed->head().headers()) {
                    headers.emplace_back(header.name(), header.value());
                }
                const auto control = cache_header(headers, "Cache-Control");
                if (cache_has_directive(control, "private") || cache_has_directive(control, "no-store") || cache_has_directive(control, "no-cache") ||
                    !cache_header(headers, "Set-Cookie").empty() ||
                    std::ranges::find(browser_cache_statuses_, *status_code_) == browser_cache_statuses_.end() ||
                    (route_ && std::ranges::any_of(route_->response_headers(), [](const auto& field) { return flexedge::route_policy::ascii_equal(field.name(), "Cache-Control"); }))) {
                    browser_cache_ttl_ = 0;
                }
            }
            const auto body = std::string_view(head_bytes_).substr(head_offset_);
            origin_response_body_status status{};
            if (captured_) {
                captured_->status_ = *status_code_;
                capture_fields(parsed->head(), *captured_);
                std::size_t header_bytes{};
                for (const auto& field : captured_->headers_) {
                    header_bytes += field.first.size() + field.second.size();
                }
                if (tunnel_ || !captured_->reservation_.try_grow(header_bytes)) {
                    captured_.reset();
                }
            }
            if (buffered_) {
                if (tunnel_) {
                    return origin_response_head_status::failure;
                }
                buffered_->status_ = *status_code_;
                capture_fields(parsed->head(), *buffered_);
                status = consume_body(body);
            } else {
                // Informational responses are relayed as received.
                output_.assign(head_bytes_, 0, head_start);
                append_head(parsed->head());
                output_.append(body);
                status = consume_body(body);
            }
            release_head();
            switch (status) {
                case origin_response_body_status::accepted:
                    return origin_response_head_status::ready;
                case origin_response_body_status::exhausted:
                    return origin_response_head_status::exhausted;
                case origin_response_body_status::failure:
                    return origin_response_head_status::failure;
            }
            return origin_response_head_status::failure;
        }
    }

    [[nodiscard]] origin_response_body_status consume_body(std::string_view bytes) {
        if (complete_) {
            return bytes.empty() ? origin_response_body_status::accepted
                                 : origin_response_body_status::failure;
        }
        switch (body_kind_) {
            case body_kind::known_length:
                if (bytes.size() > remaining_) {
                    return origin_response_body_status::failure;
                }
                remaining_ -= bytes.size();
                complete_ = remaining_ == 0;
                return retain(bytes);
            case body_kind::chunked:
                return consume_chunked(bytes);
            case body_kind::close_delimited:
                return retain(bytes);
            case body_kind::tunnel:
                return origin_response_body_status::accepted;
            case body_kind::none:
                break;
        }
        return bytes.empty() ? origin_response_body_status::accepted
                             : origin_response_body_status::failure;
    }

    // Streaming: the rewritten head and the body bytes that arrived with it.
    [[nodiscard]] std::string take_output() {
        return std::move(output_);
    }

    // Buffering: the response once complete(), or once the origin closed a
    // close-delimited body (eof_completes_response()).
    [[nodiscard]] buffered_proxy_response take_response() {
        auto response = std::move(*buffered_);
        buffered_.reset();
        return response;
    }

    // Serializes a buffered response for the client. Fails when the response budget
    // cannot also hold the framed copy; on success the reservation covers `wire` only.
    [[nodiscard]] bool frame(buffered_proxy_response& response, std::string& wire) const {
        const auto length = std::to_string(response.body_.size());
        append_status_line(wire, response.status_);
        for (const auto& [name, value] : response.headers_) {
            if (!replaced(name)) {
                append_field(wire, name, value);
            }
        }
        append_route_headers(wire);
        append_field(wire, "Content-Length", length);
        append_closing_headers(wire);
        const auto retained_bytes = response.reservation_.bytes();
        if (!response.reservation_.try_grow(wire.size() + response.body_.size())) {
            return false;
        }
        wire.reserve(wire.size() + response.body_.size());
        wire.append(response.body_);
        std::string().swap(response.body_);
        response.reservation_.shrink(retained_bytes);
        return true;
    }

    [[nodiscard]] bool complete() const noexcept {
        return complete_;
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
    static constexpr std::size_t max_buffered_body_bytes = 64 * 1024 * 1024;

    enum class body_kind { none,
        known_length,
        chunked,
        close_delimited,
        tunnel };

    static bool hop_by_hop(std::string_view name) noexcept {
        return flexedge::route_policy::ascii_equal(name, "Connection") ||
               flexedge::route_policy::ascii_equal(name, "Keep-Alive") ||
               flexedge::route_policy::ascii_equal(name, "Proxy-Connection") ||
               flexedge::route_policy::ascii_equal(name, "TE") ||
               flexedge::route_policy::ascii_equal(name, "Transfer-Encoding") ||
               flexedge::route_policy::ascii_equal(name, "Upgrade");
    }

    // Field names listed by Connection; the views borrow `head`.
    static std::vector<std::string_view> connection_options(const ruvia::http_client_response_head& head) {
        std::vector<std::string_view> options;
        for (const auto& header : head.headers()) {
            if (!flexedge::route_policy::ascii_equal(header.name(), "Connection")) {
                continue;
            }
            std::string_view value = header.value();
            while (!value.empty()) {
                const auto comma = value.find(',');
                const auto option = flexedge::route_policy::trim_ows(value.substr(0, comma));
                if (!option.empty()) {
                    options.push_back(option);
                }
                if (comma == std::string_view::npos) {
                    break;
                }
                value.remove_prefix(comma + 1);
            }
        }
        return options;
    }

    static bool connection_option(const std::vector<std::string_view>& options,
        std::string_view name) noexcept {
        return std::ranges::any_of(options, [&](std::string_view option) {
            return flexedge::route_policy::ascii_equal(name, option);
        });
    }

    // Fields the proxy sets itself.
    [[nodiscard]] bool replaced(std::string_view name) const noexcept {
        if (browser_cache_ttl_ > 0 && (flexedge::route_policy::ascii_equal(name, "Cache-Control") || flexedge::route_policy::ascii_equal(name, "Expires"))) {
            return true;
        }
        if (!cache_status_.empty() && flexedge::route_policy::ascii_equal(name, "X-Cache")) {
            return true;
        }
        if (hsts_enabled_ && flexedge::route_policy::ascii_equal(name, "Strict-Transport-Security")) {
            return true;
        }
        return route_ != nullptr &&
               std::ranges::any_of(route_->response_headers(), [&](const auto& mutation) {
                   return flexedge::route_policy::ascii_equal(name, mutation.name());
               });
    }

    static void append_field(std::string& output, std::string_view name, std::string_view value) {
        output.append(name).append(": ").append(value).append("\r\n");
    }

    static void append_status_line(std::string& output, std::uint16_t status) {
        output.append("HTTP/1.1 ").append(std::to_string(status));
        const auto reason = ruvia::http_reason_phrase(ruvia::http_status_code::from_value(status));
        if (!reason.empty()) {
            output.push_back(' ');
            output.append(reason);
        }
        output.append("\r\n");
    }

    void append_route_headers(std::string& output) const {
        if (route_ == nullptr) {
            return;
        }
        for (const auto& mutation : route_->response_headers()) {
            append_field(output, mutation.name(), mutation.value());
        }
    }

    // Each proxied response ends its client connection; an upgraded tunnel keeps the
    // origin's Connection and carries no HSTS.
    void append_closing_headers(std::string& output) const {
        if (browser_cache_ttl_ > 0) {
            append_field(output, "Cache-Control", "public, max-age=" + std::to_string(browser_cache_ttl_));
        }
        if (!cache_status_.empty()) {
            append_field(output, "X-Cache", cache_status_);
        }
        if (!tunnel_) {
            output.append("Connection: close\r\n");
            if (hsts_enabled_) {
                output.append("Strict-Transport-Security: max-age=31536000\r\n");
            }
        }
        output.append("\r\n");
    }

    // The relayed body keeps its framing, so the streaming head keeps Transfer-Encoding.
    void append_head(const ruvia::http_client_response_head& head) {
        const auto options = connection_options(head);
        append_status_line(output_, status_code_.value_or(502));
        for (const auto& header : head.headers()) {
            const auto name = header.name();
            const auto upgrade_field =
                tunnel_ && (flexedge::route_policy::ascii_equal(name, "Connection") ||
                               flexedge::route_policy::ascii_equal(name, "Upgrade"));
            const auto framing_field = flexedge::route_policy::ascii_equal(name, "Transfer-Encoding");
            if ((!upgrade_field && !framing_field &&
                    (hop_by_hop(name) || connection_option(options, name))) ||
                replaced(name)) {
                continue;
            }
            append_field(output_, name, header.value());
        }
        append_route_headers(output_);
        append_closing_headers(output_);
    }

    // The buffered body is decoded and re-framed, so its framing fields and the trailer
    // declaration are dropped with the hop-by-hop fields.
    void capture_fields(const ruvia::http_client_response_head& head, buffered_proxy_response& target) {
        const auto options = connection_options(head);
        for (const auto& header : head.headers()) {
            const auto name = header.name();
            if (hop_by_hop(name) || connection_option(options, name) ||
                flexedge::route_policy::ascii_equal(name, "Content-Length") ||
                flexedge::route_policy::ascii_equal(name, "Trailer")) {
                continue;
            }
            target.headers_.emplace_back(name, header.value());
        }
    }

    void release_head() noexcept {
        if (buffered_) {
            buffered_->reservation_.shrink(head_bytes_.size());
        }
        std::string().swap(head_bytes_);
        head_offset_ = 0;
    }

    void configure_body(const ruvia::http1_client_response_plan& plan) {
        origin_reusable_ = plan_allows_reuse(plan);
        if (tunnel_) {
            body_kind_ = body_kind::tunnel;
            return;
        }
        const auto* zero = plan.zero_content();
        if (const auto* known = zero != nullptr ? zero->known_length() : plan.known_length()) {
            body_kind_ = body_kind::known_length;
            remaining_ = known->content_length();
            complete_ = remaining_ == 0;
            return;
        }
        if (plan.chunked() != nullptr || (zero != nullptr && zero->chunked() != nullptr)) {
            body_kind_ = body_kind::chunked;
            chunked_.emplace(ruvia::http1_chunked_body_decoder_config{
                .trailer_role_ = ruvia::http1_chunk_trailer_role::response,
            });
            return;
        }
        if (plan.close_delimited() != nullptr || zero != nullptr) {
            body_kind_ = body_kind::close_delimited;
            return;
        }
        body_kind_ = body_kind::none;
        complete_ = true;
    }

    // Body bytes are relayed by the caller when streaming and kept when buffering.
    [[nodiscard]] origin_response_body_status retain(std::string_view bytes) {
        if (captured_ && !bytes.empty()) {
            if (bytes.size() > capture_limit_ - (std::min)(capture_limit_, captured_->body_.size()) || !captured_->reservation_.try_grow(bytes.size())) {
                captured_.reset();
            } else {
                captured_->body_.append(bytes);
            }
        }
        if (!buffered_ || bytes.empty()) {
            return origin_response_body_status::accepted;
        }
        if (bytes.size() > max_buffered_body_bytes - buffered_->body_.size()) {
            return origin_response_body_status::failure;
        }
        if (!buffered_->reservation_.try_grow(bytes.size())) {
            return origin_response_body_status::exhausted;
        }
        buffered_->body_.append(bytes);
        return origin_response_body_status::accepted;
    }

    // Ruvia leaves incomplete chunk framing unconsumed; it waits in chunk_framing_ until
    // the rest arrives.
    [[nodiscard]] origin_response_body_status consume_chunked(std::string_view bytes) {
        auto input = bytes;
        if (!chunk_framing_.empty()) {
            chunk_framing_.append(bytes);
            input = chunk_framing_;
        }
        std::size_t offset{};
        for (;;) {
            const auto step = chunked_->decode(input.substr(offset));
            if (const auto* chunk = step.body_chunk()) {
                if (const auto status = retain(chunk->bytes());
                    status != origin_response_body_status::accepted) {
                    return status;
                }
                offset += chunk->consumed_bytes();
                continue;
            }
            if (const auto* end = step.complete()) {
                offset += end->consumed_bytes();
                complete_ = true;
                std::string().swap(chunk_framing_);
                return offset == input.size() ? origin_response_body_status::accepted
                                              : origin_response_body_status::failure;
            }
            if (step.failure() != nullptr) {
                return origin_response_body_status::failure;
            }
            offset += step.need_more()->consumed_bytes();
            break;
        }
        if (input.data() == chunk_framing_.data()) {
            chunk_framing_.erase(0, offset);
        } else {
            chunk_framing_.assign(input.substr(offset));
        }
        return origin_response_body_status::accepted;
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
    std::optional<buffered_proxy_response> buffered_;
    std::optional<buffered_proxy_response> captured_;
    std::size_t capture_limit_{};
    std::int64_t browser_cache_ttl_{};
    std::vector<std::int64_t> browser_cache_statuses_{};
    std::string cache_status_;
    body_kind body_kind_{body_kind::none};
    std::size_t remaining_{};
    std::optional<ruvia::http1_chunked_body_decoder> chunked_;
    std::string chunk_framing_;
    std::optional<std::uint16_t> status_code_;
    bool hsts_enabled_{};
    const evaluated_route_policy* route_{};
    bool tunnel_{};
    bool complete_{};
    bool origin_reusable_{};
};

}  // namespace flexedge::node
