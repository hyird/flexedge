#pragma once

#include <ruvia/http/http1_chunked_body_decoder.h>
#include <ruvia/http/http1_client_exchange_state.h>
#include <ruvia/http/http1_client_response_parser.h>
#include <ruvia/http/http1_close_policy.h>
#include <ruvia/http/http_client_response_head.h>
#include <ruvia/http/http_status.h>

#include <algorithm>
#include <charconv>
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
#include "node/data/cache_response_head.h"
#include "node/data/origin_request_codec.h"
#include "node/data/response_cache.h"
#include "node/data/response_compression.h"

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
// Every read emits bounded wire bytes through take_output(). Compressed responses
// decode transfer framing, encode incrementally and emit fresh chunk framing. Cache
// output contains only the current read; its worker-owned consumer handles storage.
class origin_response_codec final {
public:
    // `route` and the immutable route program backing its header views must outlive
    // the codec, including its compression policy and borrowed request target.
    origin_response_codec(ruvia::http1_client_exchange_state response_exchange, bool hsts_enabled,
        const evaluated_route_policy* route)
        : parser_(std::make_unique<ruvia::http1_client_response_parser>(std::move(response_exchange))),
          hsts_enabled_(hsts_enabled),
          route_(route) {}

    void set_compression(const v2::Website& website, std::string_view target,
        std::string_view accept_encoding, buffered_bytes_lease reservation) {
        compression_website_ = &website;
        compression_target_ = target;
        accept_encoding_ = accept_encoding;
        compression_reservation_ = std::move(reservation);
    }

    void capture_for_cache() {
        capture_enabled_ = true;
    }
    void stop_cache_capture() noexcept {
        capture_enabled_ = false;
        captured_.reset();
        std::string().swap(capture_output_);
    }
    [[nodiscard]] const cache_response_head* cache_head() const noexcept {
        return captured_ ? &*captured_ : nullptr;
    }
    [[nodiscard]] std::string take_cache_output() {
        return std::exchange(capture_output_, {});
    }
    void set_browser_cache_ttl(std::int64_t seconds, const std::vector<std::int64_t>& statuses) {
        browser_cache_ttl_ = seconds;
        browser_cache_statuses_ = statuses;
    }
    void set_cache_status(std::string value) {
        cache_status_ = std::move(value);
    }
    void set_cache_rule(const flexedge::cache::rule& rule) noexcept {
        cache_rule_ = &rule;
    }
    [[nodiscard]] std::string_view cache_bypass_reason() const noexcept {
        return cache_bypass_reason_;
    }

    [[nodiscard]] origin_response_head_status consume_head(std::string_view bytes) {
        try {
            return consume_head_impl(bytes);
        } catch (const std::bad_alloc&) {
            return origin_response_head_status::exhausted;
        } catch (const ruvia::http_content_encoder_error&) {
            return origin_response_head_status::failure;
        }
    }

    [[nodiscard]] origin_response_body_status consume_body(std::string_view bytes) {
        try {
            return consume_body_impl(bytes);
        } catch (const std::bad_alloc&) {
            return origin_response_body_status::exhausted;
        } catch (const ruvia::http_content_encoder_error&) {
            return origin_response_body_status::failure;
        }
    }

    [[nodiscard]] origin_response_body_status consume_eof() {
        if (!eof_completes_response()) {
            return origin_response_body_status::failure;
        }
        try {
            finish_body();
            return origin_response_body_status::accepted;
        } catch (const std::bad_alloc&) {
            return origin_response_body_status::exhausted;
        } catch (const ruvia::http_content_encoder_error&) {
            return origin_response_body_status::failure;
        }
    }

    [[nodiscard]] std::string take_output() {
        return std::exchange(output_, {});
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
    [[nodiscard]] origin_response_head_status consume_head_impl(std::string_view bytes) {
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
            if (tunnel_ || is_event_stream_content_type(response_header(parsed->head(), "Content-Type"))) {
                cache_bypass_reason_ = tunnel_ ? "origin_upgrade" : "origin_event_stream";
                stop_cache_capture();
                browser_cache_ttl_ = 0;
                if (!cache_status_.empty()) {
                    cache_status_ = "BYPASS";
                }
            }
            configure_compression(parsed->head(), parsed->plan());
            if (browser_cache_ttl_ > 0) {
                cache_headers headers;
                for (const auto& header : parsed->head().headers()) {
                    headers.emplace_back(header.name(), header.value());
                }
                const auto control = cache_header(headers, "Cache-Control");
                if (cache_has_directive(control, "private") || cache_has_directive(control, "no-store") ||
                    (cache_has_directive(control, "no-cache") && (!cache_rule_ || cache_rule_->skip_no_cache_)) ||
                    !cache_header(headers, "Set-Cookie").empty() ||
                    std::ranges::find(browser_cache_statuses_, *status_code_) == browser_cache_statuses_.end() ||
                    (route_ && std::ranges::any_of(route_->response_headers(), [](const auto& field) { return flexedge::route_policy::ascii_equal(field.name(), "Cache-Control"); }))) {
                    browser_cache_ttl_ = 0;
                }
            }
            const auto body = std::string_view(head_bytes_).substr(head_offset_);
            if (capture_enabled_) {
                captured_.emplace();
                captured_->status_ = *status_code_;
                capture_fields(parsed->head(), *captured_);
                if (cache_rule_ && *status_code_ != 304) {
                    cache_bypass_reason_ = response_cache::response_bypass_reason(*cache_rule_, *captured_);
                    if (!cache_bypass_reason_.empty()) {
                        cache_status_ = "BYPASS";
                        stop_cache_capture();
                    }
                }
            }
            // Informational responses are relayed as received.
            output_.assign(head_bytes_, 0, head_start);
            append_head(parsed->head());
            const auto status = consume_body_impl(body);
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

    [[nodiscard]] origin_response_body_status consume_body_impl(std::string_view bytes) {
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
                retain(bytes);
                if (!compressed_) {
                    output_.append(bytes);
                }
                if (remaining_ == 0) {
                    finish_body();
                }
                return origin_response_body_status::accepted;
            case body_kind::chunked:
                if (!compressed_) {
                    output_.append(bytes);
                }
                return consume_chunked(bytes);
            case body_kind::close_delimited:
                retain(bytes);
                if (!compressed_) {
                    output_.append(bytes);
                }
                return origin_response_body_status::accepted;
            case body_kind::tunnel:
                output_.append(bytes);
                return origin_response_body_status::accepted;
            case body_kind::none:
                break;
        }
        return bytes.empty() ? origin_response_body_status::accepted
                             : origin_response_body_status::failure;
    }

    static constexpr std::size_t max_head_bytes = 1024 * 1024;

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

    std::string response_header(const ruvia::http_client_response_head& head, std::string_view name) const {
        std::string value;
        bool replaced_by_route{};
        if (route_) {
            for (const auto& field : route_->response_headers()) {
                if (flexedge::route_policy::ascii_equal(field.name(), name)) {
                    if (!value.empty()) {
                        value.append(", ");
                    }
                    value.append(field.value());
                    replaced_by_route = true;
                }
            }
        }
        if (!replaced_by_route) {
            for (const auto& field : head.headers()) {
                if (flexedge::route_policy::ascii_equal(field.name(), name)) {
                    if (!value.empty()) {
                        value.append(", ");
                    }
                    value.append(field.value());
                }
            }
        }
        return value;
    }
    void configure_compression(const ruvia::http_client_response_head& head,
        const ruvia::http1_client_response_plan& plan) {
        if (!compression_website_ || tunnel_ || plan.without_content() || plan.zero_content()) {
            return;
        }
        const auto header = [&](std::string_view name) { return response_header(head, name); };
        // A preceding transfer coding is not a representation we can compress or cache.
        const auto transfer = header("Transfer-Encoding");
        if (!transfer.empty() && !flexedge::route_policy::ascii_equal(
                                     flexedge::route_policy::trim_ows(transfer), "chunked")) {
            stop_cache_capture();
            return;
        }
        const auto choice = select_response_compression(*compression_website_, compression_target_,
            accept_encoding_, *status_code_,
            plan.known_length() ? std::optional<std::size_t>(plan.known_length()->content_length())
                                : std::nullopt,
            header);
        if (choice.vary_) {
            for (const auto& field : head.headers()) {
                if (flexedge::route_policy::ascii_equal(field.name(), "Vary") && !replaced(field.name())) {
                    if (!vary_.empty()) {
                        vary_.append(", ");
                    }
                    vary_.append(field.value());
                }
            }
            if (route_) {
                for (const auto& field : route_->response_headers()) {
                    if (flexedge::route_policy::ascii_equal(field.name(), "Vary")) {
                        if (!vary_.empty()) {
                            vary_.append(", ");
                        }
                        vary_.append(field.value());
                    }
                }
            }
            const auto tokens = cache_tokens(vary_);
            if (std::ranges::find(tokens, "accept-encoding") == tokens.end() &&
                std::ranges::find(tokens, "*") == tokens.end()) {
                if (!vary_.empty()) {
                    vary_.append(", ");
                }
                vary_.append("Accept-Encoding");
            }
        }
        if (choice.coding_ != ruvia::http_content_coding::identity) {
            try {
                compressor_ = std::make_unique<response_compressor>(choice.coding_, std::move(compression_reservation_));
                compressed_ = true;
                content_encoding_ = ruvia::http_content_coding_token(choice.coding_);
            } catch (const std::bad_alloc&) {
                // No response bytes have been sent. Serve the original representation.
            } catch (const ruvia::http_content_encoder_error&) {
            }
        }
    }

    // Apply representation changes equally to client headers and cache metadata.
    void representation_field(std::string_view name, std::string_view value, const auto& emit) const {
        if (!vary_.empty() && flexedge::route_policy::ascii_equal(name, "Vary")) {
            return;
        }
        if (compressed_) {
            if (flexedge::route_policy::ascii_equal(name, "Content-Length") ||
                flexedge::route_policy::ascii_equal(name, "Transfer-Encoding") ||
                flexedge::route_policy::ascii_equal(name, "Trailer") ||
                flexedge::route_policy::ascii_equal(name, "Content-Encoding") ||
                flexedge::route_policy::ascii_equal(name, "Content-MD5") ||
                flexedge::route_policy::ascii_equal(name, "Digest") ||
                flexedge::route_policy::ascii_equal(name, "Content-Digest") ||
                flexedge::route_policy::ascii_equal(name, "Repr-Digest") ||
                flexedge::route_policy::ascii_equal(name, "Accept-Ranges")) {
                return;
            }
            if (flexedge::route_policy::ascii_equal(name, "ETag") && value.starts_with('"')) {
                emit(name, "W/" + std::string(value));
                return;
            }
        }
        emit(name, value);
    }

    void representation_fields(const auto& emit) const {
        if (!vary_.empty()) {
            emit("Vary", vary_);
        }
        if (compressed_) {
            emit("Content-Encoding", content_encoding_);
        }
    }

    // Fields the proxy sets itself.
    [[nodiscard]] bool replaced(std::string_view name, bool for_cache = false) const noexcept {
        if (!for_cache && browser_cache_ttl_ > 0 && (flexedge::route_policy::ascii_equal(name, "Cache-Control") || flexedge::route_policy::ascii_equal(name, "Expires"))) {
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
            representation_field(mutation.name(), mutation.value(),
                [&](auto name, auto value) { append_field(output, name, value); });
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
            representation_field(name, header.value(),
                [&](auto key, auto value) { append_field(output_, key, value); });
        }
        append_route_headers(output_);
        representation_fields([&](auto name, auto value) { append_field(output_, name, value); });
        if (compressed_) {
            append_field(output_, "Transfer-Encoding", "chunked");
        }
        append_closing_headers(output_);
    }

    // Cache bytes exclude transfer framing, so its fields and the trailer declaration
    // are dropped with the hop-by-hop fields.
    void capture_fields(const ruvia::http_client_response_head& head, cache_response_head& target) {
        const auto options = connection_options(head);
        for (const auto& header : head.headers()) {
            const auto name = header.name();
            if (hop_by_hop(name) || connection_option(options, name) ||
                flexedge::route_policy::ascii_equal(name, "Content-Length") ||
                flexedge::route_policy::ascii_equal(name, "Trailer") || replaced(name, true)) {
                continue;
            }
            representation_field(name, header.value(),
                [&](auto key, auto value) { target.headers_.emplace_back(key, value); });
        }
        if (route_) {
            for (const auto& field : route_->response_headers()) {
                representation_field(field.name(), field.value(),
                    [&](auto key, auto value) { target.headers_.emplace_back(key, value); });
            }
        }
        representation_fields([&](auto name, auto value) { target.headers_.emplace_back(name, value); });
    }

    void release_head() noexcept {
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

    void capture(std::string_view bytes) {
        if (captured_) {
            capture_output_.append(bytes);
        }
    }

    void append_chunk(std::string_view bytes) {
        capture(bytes);
        if (bytes.empty()) {
            return;
        }
        char length[2 * sizeof(std::size_t)];
        const auto result = std::to_chars(length, length + sizeof(length), bytes.size(), 16);
        output_.append(length, result.ptr).append("\r\n").append(bytes).append("\r\n");
    }

    void retain(std::string_view bytes) {
        if (compressor_) {
            append_chunk(compressor_->write(bytes));
            compressor_->clear_output();
        } else {
            capture(bytes);
        }
    }

    void finish_body() {
        if (compressor_) {
            append_chunk(compressor_->finish());
            output_.append("0\r\n\r\n");
            compressor_.reset();
        }
        complete_ = true;
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
                retain(chunk->bytes());
                offset += chunk->consumed_bytes();
                continue;
            }
            if (const auto* end = step.complete()) {
                offset += end->consumed_bytes();
                if (offset != input.size()) {
                    return origin_response_body_status::failure;
                }
                finish_body();
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
    const v2::Website* compression_website_{};
    std::string_view compression_target_;
    std::string_view accept_encoding_;
    buffered_bytes_lease compression_reservation_;
    std::unique_ptr<response_compressor> compressor_;
    std::string content_encoding_;
    std::string vary_;
    bool compressed_{};
    std::optional<cache_response_head> captured_;
    std::string capture_output_{};
    bool capture_enabled_{};
    std::int64_t browser_cache_ttl_{};
    std::vector<std::int64_t> browser_cache_statuses_{};
    std::string cache_status_;
    const flexedge::cache::rule* cache_rule_{};
    std::string_view cache_bypass_reason_{};
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
