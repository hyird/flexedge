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

    // `buffer` receives incremental bytes for this request; once the head is accepted its
    // bytes are owned by `head_bytes_` and `buffer` retains only body or post-message bytes.
    [[nodiscard]] http1_request_read_status consume(std::string& buffer) {
        if (header_bytes_ == 0) {
            const auto status = consume_head(buffer);
            if (status != http1_request_read_status::complete) {
                return status;
            }
        }
        return consume_body(buffer);
    }

    [[nodiscard]] const ruvia::http_request& request() const noexcept {
        return state_.request_;
    }

    [[nodiscard]] bool has_body() const noexcept {
        return known_length_.has_value() || chunked_;
    }

    [[nodiscard]] bool expects_continue() const noexcept {
        if (header_bytes_ == 0) {
            return false;
        }
        const auto plan = state_.body_plan_.expectation_plan(
            ruvia::http_unsupported_expectation_policy::ignore);
        return plan.send_continue() != nullptr;
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

    [[nodiscard]] std::string_view unconsumed_bytes() const noexcept {
        return unconsumed_bytes_;
    }

private:
    http1_request_read_status consume_head(std::string& buffer) {
        if (head_bytes_.empty()) {
            // Parsed fields borrow this storage; transfer it before parsing and
            // retain it independently of the body buffer's later reallocations.
            initial_input_capacity_ = buffer.capacity();
            head_bytes_.swap(buffer);
        } else {
            head_bytes_.append(buffer);
            buffer.clear();
        }
        parser_.parse_head(head_bytes_, state_, header_search_offset_);
        if (state_.failure() != nullptr) {
            return http1_request_read_status::bad_request;
        }
        const auto* head = state_.head_ready();
        if (head == nullptr) {
            if (head_bytes_.size() >= max_request_bytes_) {
                return http1_request_read_status::too_large;
            }
            header_search_offset_ = head_bytes_.size();
            return http1_request_read_status::need_more;
        }
        header_bytes_ = head->header_bytes();
        const auto* known = state_.body_plan_.known_length();
        if (known != nullptr) {
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
        if (known != nullptr && known->content_length() > head_bytes_.size() - header_bytes_) {
            // Preserve the feed buffer's growth granularity for incomplete fixed bodies,
            // never preallocate a peer's declared Content-Length.
            const auto capacity = initial_input_capacity_ < known->content_length()
                                      ? initial_input_capacity_
                                      : known->content_length();
            buffer.reserve(capacity);
        }
        buffer.assign(std::string_view(head_bytes_).substr(header_bytes_));
        head_bytes_.resize(header_bytes_);
        return http1_request_read_status::complete;
    }

    http1_request_read_status consume_body(std::string& buffer) {
        if (known_length_) {
            if (buffer.size() < *known_length_) {
                return http1_request_read_status::need_more;
            }
            body_ = std::string_view(buffer).substr(0, *known_length_);
            unconsumed_bytes_ = std::string_view(buffer).substr(*known_length_);
            message_bytes_ = header_bytes_ + *known_length_;
            return http1_request_read_status::complete;
        }
        if (!chunked_) {
            body_ = {};
            unconsumed_bytes_ = buffer;
            message_bytes_ = header_bytes_;
            return http1_request_read_status::complete;
        }
        std::size_t offset{};
        for (;;) {
            const auto available = std::string_view(buffer).substr(offset);
            const auto step = decoder_->decode(available);
            if (const auto* chunk = step.body_chunk()) {
                append_decoded(chunk->bytes());
                offset += chunk->consumed_bytes();
                chunked_bytes_ += chunk->consumed_bytes();
                continue;
            }
            if (const auto* complete = step.complete()) {
                offset += complete->consumed_bytes();
                chunked_bytes_ += complete->consumed_bytes();
                buffer.erase(0, offset);
                body_ = decoded_body_;
                unconsumed_bytes_ = buffer;
                message_bytes_ = header_bytes_ + chunked_bytes_;
                return http1_request_read_status::complete;
            }
            if (step.failure() != nullptr) {
                buffer.erase(0, offset);
                return http1_request_read_status::bad_request;
            }
            const auto consumed = step.need_more()->consumed_bytes();
            offset += consumed;
            chunked_bytes_ += consumed;
            buffer.erase(0, offset);
            if (header_bytes_ + chunked_bytes_ + buffer.size() >= max_request_bytes_) {
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
    std::string head_bytes_;
    std::size_t initial_input_capacity_{};
    std::size_t header_search_offset_{};
    std::size_t header_bytes_{};
    std::optional<std::size_t> known_length_;
    bool chunked_{};
    std::optional<ruvia::http1_chunked_body_decoder> decoder_;
    std::size_t chunked_bytes_{};
    std::string decoded_body_;
    bool decoded_body_exceeded_{};
    std::string_view body_;
    std::string_view unconsumed_bytes_;
    std::size_t message_bytes_{};
};

}  // namespace flexedge::node
