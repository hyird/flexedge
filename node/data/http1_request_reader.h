#pragma once

#include <ruvia/http/http1_chunked_body_decoder.h>
#include <ruvia/http/http1_request_body_plan.h>
#include <ruvia/http/http1_server_request_parser.h>
#include <ruvia/http/http_limits.h>
#include <ruvia/http/http_request.h>
#include <ruvia/http/protocol_byte_limit.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace flexedge::node {

enum class http1_request_read_status { need_more,
    complete,
    bad_request,
    too_large };

// Frames one buffered downstream HTTP/1 request. Ruvia parses only the request head;
// this reader slices Content-Length bodies, decodes chunked bodies, and applies the
// node's request budget. Malformed framing, a declared body above Ruvia's buffered body
// ceiling, and oversized chunk framing are bad requests; a message that cannot complete
// within max_request_bytes is too large.
class http1_request_reader final {
public:
    explicit http1_request_reader(std::size_t max_request_bytes) noexcept
        : max_request_bytes_(max_request_bytes) {}

    http1_request_reader(const http1_request_reader&) = delete;
    http1_request_reader& operator=(const http1_request_reader&) = delete;

    // `buffer` holds every byte received for this request, including bytes from earlier
    // calls. After complete, request() and body() borrow `buffer` until it changes.
    [[nodiscard]] http1_request_read_status consume(std::string_view buffer) {
        if (header_bytes_ == 0) {
            const auto status = consume_head(buffer);
            if (status != http1_request_read_status::complete) {
                return status;
            }
        }
        const auto status = consume_body(buffer);
        if (status == http1_request_read_status::complete) {
            // Header views borrow the caller buffer, which may have moved since the head
            // was first accepted. The head bytes are unchanged, so reparsing is stable.
            parser_.parse_head(buffer, state_);
            if (state_.head_ready() == nullptr) {
                return http1_request_read_status::bad_request;
            }
            if (known_length_) {
                body_ = buffer.substr(header_bytes_, *known_length_);
            } else if (chunked_) {
                body_ = decoded_body_;
            }
        }
        return status;
    }

    [[nodiscard]] const ruvia::http_request& request() const noexcept {
        return state_.request_;
    }

    [[nodiscard]] bool has_body() const noexcept {
        return known_length_.has_value() || chunked_;
    }

    // The framed content; empty without a body. A chunked body whose decoded size exceeds
    // max_request_bytes is framed but not usable, which is reported as std::nullopt.
    [[nodiscard]] std::optional<std::string_view> body() const noexcept {
        if (decoded_body_exceeded_) {
            return std::nullopt;
        }
        return body_;
    }

    // Header and framed content bytes; bytes after this belong to the next message.
    [[nodiscard]] std::size_t message_bytes() const noexcept {
        return message_bytes_;
    }

private:
    http1_request_read_status consume_head(std::string_view buffer) {
        parser_.parse_head(buffer, state_, header_search_offset_);
        if (state_.failure() != nullptr) {
            return http1_request_read_status::bad_request;
        }
        const auto* head = state_.head_ready();
        if (head == nullptr) {
            if (buffer.size() >= max_request_bytes_) {
                return http1_request_read_status::too_large;
            }
            header_search_offset_ = buffer.size();
            return http1_request_read_status::need_more;
        }
        header_bytes_ = head->header_bytes();
        if (const auto* known = state_.body_plan_.known_length()) {
            const auto content_length = known->content_length();
            if (content_length > ruvia::default_max_buffered_body_bytes) {
                return http1_request_read_status::bad_request;
            }
            if (header_bytes_ + content_length > max_request_bytes_) {
                return http1_request_read_status::too_large;
            }
            known_length_ = content_length;
        } else if (state_.body_plan_.chunked() != nullptr) {
            chunked_ = true;
            decoder_.emplace(ruvia::http1_chunked_body_decoder_config{
                .body_limit_ =
                    ruvia::protocol_byte_limit::limited(ruvia::default_max_buffered_body_bytes),
                .trailer_role_ = ruvia::http1_chunk_trailer_role::request,
            });
        }
        return http1_request_read_status::complete;
    }

    http1_request_read_status consume_body(std::string_view buffer) {
        if (known_length_) {
            if (buffer.size() - header_bytes_ < *known_length_) {
                return http1_request_read_status::need_more;
            }
            message_bytes_ = header_bytes_ + *known_length_;
            return http1_request_read_status::complete;
        }
        if (!chunked_) {
            message_bytes_ = header_bytes_;
            return http1_request_read_status::complete;
        }
        for (;;) {
            const auto available = buffer.substr(header_bytes_ + chunked_bytes_);
            const auto step = decoder_->decode(available);
            if (const auto* chunk = step.body_chunk()) {
                append_decoded(chunk->bytes());
                chunked_bytes_ += chunk->consumed_bytes();
                continue;
            }
            if (const auto* complete = step.complete()) {
                chunked_bytes_ += complete->consumed_bytes();
                message_bytes_ = header_bytes_ + chunked_bytes_;
                return http1_request_read_status::complete;
            }
            if (step.failure() != nullptr) {
                return http1_request_read_status::bad_request;
            }
            chunked_bytes_ += step.need_more()->consumed_bytes();
            if (buffer.size() >= max_request_bytes_) {
                return http1_request_read_status::too_large;
            }
            return http1_request_read_status::need_more;
        }
    }

    void append_decoded(std::string_view bytes) {
        if (decoded_body_exceeded_) {
            return;
        }
        if (bytes.size() > max_request_bytes_ - decoded_body_.size()) {
            decoded_body_exceeded_ = true;
            std::string().swap(decoded_body_);
            return;
        }
        decoded_body_.append(bytes);
    }

    std::size_t max_request_bytes_;
    ruvia::http1_server_request_parser parser_;
    ruvia::http1_server_request_parse_state state_;
    std::size_t header_search_offset_{};
    std::size_t header_bytes_{};
    std::optional<std::size_t> known_length_;
    bool chunked_{};
    std::optional<ruvia::http1_chunked_body_decoder> decoder_;
    std::size_t chunked_bytes_{};
    std::string decoded_body_;
    bool decoded_body_exceeded_{};
    std::string_view body_;
    std::size_t message_bytes_{};
};

}  // namespace flexedge::node
