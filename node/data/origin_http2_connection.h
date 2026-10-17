#pragma once

#include <ruvia/http/http2_connection.h>

#include <array>
#include <charconv>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <variant>

#include <asio/post.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/write.hpp>

#include "node/data/http1_request_reader.h"
#include "node/data/origin_request_codec.h"

namespace flexedge::node {

// Worker-owned HTTP/2 connection. The existing response pipeline receives an
// HTTP/1 framing view; HPACK, flow control and stream state stay in Ruvia.
class origin_http2_connection final : public std::enable_shared_from_this<origin_http2_connection> {
public:
    using tls_stream = asio::ssl::stream<asio::ip::tcp::socket>;
    using completion = std::function<void(std::error_code, std::size_t)>;
    template <class stream_type>
    explicit origin_http2_connection(stream_type stream, bool secure)
        : stream_(std::move(stream)),
          secure_(secure),
          protocol_(ruvia::http2_connection::client()) {}
    void start() {
        read_network();
        flush();
    }
    bool reusable() const {
        return !closed_ && !goaway_ && response_complete_ && !upload_pending_ &&
               !write_handler_ && !read_handler_ && queue_.empty() && !writing_ &&
               !protocol_.wants_write() && !protocol_.has_queued_data(stream_id_);
    }
    void request(std::string_view bytes, completion handler) {
        if (closed_ || goaway_ || (stream_id_ != 0 && !reusable())) {
            complete(std::move(handler), asio::error::operation_aborted, 0);
            return;
        }
        http1_request_reader reader(2 * 1024 * 1024);
        std::string input(bytes);
        if (reader.consume(input) != http1_request_read_status::complete) {
            complete(std::move(handler), asio::error::invalid_argument, 0);
            return;
        }
        const auto& head = reader.request();
        method_ = head.method();
        body_ = std::string(reader.body().value_or(""));
        body_offset_ = 0;
        std::vector<std::pair<std::string, std::string>> owned_headers;
        for (const auto& field : head.headers()) {
            if (!request_hop_by_hop_header(field.name())) {
                std::string name(field.name());
                std::ranges::transform(name, name.begin(), [](unsigned char value) { return flexedge::route_policy::lower_ascii(value); });
                owned_headers.emplace_back(std::move(name), field.value());
            }
        }
        std::vector<ruvia::http_header_view> headers;
        for (const auto& [name, value] : owned_headers) {
            headers.emplace_back(name, value);
        }
        const auto authority = head.header("Host").value_or("");
        const auto submitted = protocol_.submit_request_head({.method_ = std::string_view(method_),
            .scheme_ = secure_ ? "https" : "http",
            .authority_ = authority,
            .target_ = head.target(),
            .headers_ = headers,
            .content_ = reader.has_body() ? ruvia::http2_request_content::known_length(body_.size()) : ruvia::http2_request_content::none()});
        if (!submitted.submitted()) {
            complete(std::move(handler), asio::error::fault, 0);
            close();
            return;
        }
        stream_id_ = submitted.submitted()->stream_id();
        response_complete_ = false;
        upload_pending_ = true;
        chunked_ = false;
        write_size_ = bytes.size();
        write_handler_ = std::move(handler);
        // The adapter owns the entire request now. Let the response reader run
        // even when the peer withholds the upload's DATA window (RFC 9113 8.1).
        asio::post(executor(), [self = shared_from_this()] {
            if (auto handler = std::move(self->write_handler_)) {
                handler({}, self->write_size_);
            }
        });
        pump();
    }
    void read(asio::mutable_buffer buffer, completion handler) {
        if (read_handler_) {
            complete(std::move(handler), asio::error::already_started, 0);
            return;
        }
        read_buffer_ = buffer;
        read_handler_ = std::move(handler);
        deliver();
    }
    void close() noexcept {
        if (closed_) {
            return;
        }
        closed_ = true;
        upload_pending_ = false;
        body_.clear();
        std::visit([](auto& stream) { std::error_code ignored;stream.lowest_layer().close(ignored); }, stream_);
        auto write = std::move(write_handler_), read = std::move(read_handler_);
        try {
            if (write) {
                complete(std::move(write), asio::error::operation_aborted, 0);
            }
            if (read) {
                complete(std::move(read), asio::error::operation_aborted, 0);
            }
        } catch (...) {
        }
    }

private:
    struct segment final {
        std::string bytes_;
        std::size_t offset_{};
        std::optional<ruvia::http2_received_data_credit> credit_{};
    };
    asio::any_io_executor executor() {
        return std::visit([](auto& stream) -> asio::any_io_executor { return stream.get_executor(); }, stream_);
    }
    void complete(completion handler, std::error_code error, std::size_t size) {
        asio::post(executor(), [handler = std::move(handler), error, size]() mutable {
            handler(error, size);
        });
    }
    void deliver() {
        if (!read_handler_) {
            return;
        }
        if (queue_.empty()) {
            if (closed_) {
                auto handler = std::move(read_handler_);
                complete(std::move(handler), asio::error::eof, 0);
            }
            return;
        }
        auto& entry = queue_.front();
        const auto size = (std::min)(read_buffer_.size(), entry.bytes_.size() - entry.offset_);
        std::memcpy(read_buffer_.data(), entry.bytes_.data() + entry.offset_, size);
        entry.offset_ += size;
        buffered_ -= size;
        if (entry.offset_ == entry.bytes_.size()) {
            queue_.pop_front();
        }
        auto handler = std::move(read_handler_);
        // Posting preserves Asio's no-inline completion contract.
        asio::post(executor(), [self = shared_from_this(), handler = std::move(handler), size]() mutable {handler({},size);self->pump(); });
    }
    void append(std::string bytes, std::optional<ruvia::http2_received_data_credit> credit = {}) {
        buffered_ += bytes.size();
        queue_.push_back({std::move(bytes), 0, std::move(credit)});
    }
    void events() {
        while (auto event = protocol_.next_event()) {
            if (const auto* head = event->response_head()) {
                if (head->stream_id() != stream_id_) {
                    continue;
                }
                const auto status = head->head().status().value();
                std::string wire = "HTTP/1.1 " + std::to_string(status) + " " + std::string(ruvia::http_reason_phrase(head->head().status())) + "\r\n";
                bool length{};
                for (const auto& field : head->head().headers()) {
                    if (request_hop_by_hop_header(field.name()) && !flexedge::route_policy::ascii_equal(field.name(), "Content-Length")) {
                        continue;
                    }
                    length = length || flexedge::route_policy::ascii_equal(field.name(), "Content-Length");
                    wire.append(field.name()).append(": ").append(field.value()).append("\r\n");
                }
                chunked_ = !length && method_ != "HEAD" && status != 204 && status != 304;
                if (chunked_) {
                    wire.append("Transfer-Encoding: chunked\r\n");
                }
                wire.append("\r\n");
                append(std::move(wire));
            } else if (auto* data = event->message_body_chunk()) {
                if (data->stream_id() != stream_id_) {
                    continue;
                }
                std::string bytes;
                if (chunked_) {
                    char hex[32];
                    const auto converted = std::to_chars(hex, hex + sizeof(hex), data->bytes().size(), 16);
                    bytes.append(hex, converted.ptr).append("\r\n");
                }
                bytes.append(data->bytes());
                if (chunked_) {
                    bytes.append("\r\n");
                }
                append(std::move(bytes), data->take_credit());
            } else if (const auto* end = event->message_end()) {
                if (end->stream_id() != stream_id_) {
                    continue;
                }
                if (chunked_) {
                    append("0\r\n\r\n");
                }
                response_complete_ = true;
            } else if (const auto* reset = event->stream_closed()) {
                if (reset->stream_id() != stream_id_) {
                    continue;
                }
                if (!response_complete_) {
                    close();
                    return;
                }
                // END_STREAM already made the response valid. A later reset
                // terminates only the remaining upload, not the queued response.
                upload_pending_ = false;
                body_.clear();
            } else if (event->goaway()) {
                goaway_ = true;
            } else if (event->request_unprocessed()) {
                close();
                return;
            }
        }
        deliver();
    }
    void pump() {
        if (closed_) {
            return;
        }
        while (upload_pending_ && body_offset_ < body_.size() && !protocol_.has_queued_data(stream_id_)) {
            const auto size = (std::min)(std::size_t{16384}, body_.size() - body_offset_);
            const auto result = protocol_.submit_data(stream_id_, std::string_view(body_).substr(body_offset_, size),
                body_offset_ + size == body_.size() ? ruvia::http2_end_stream::end_stream : ruvia::http2_end_stream::keep_open);
            if (result == ruvia::http2_data_submit_status::backpressured) {
                break;
            }
            if (result != ruvia::http2_data_submit_status::accepted && result != ruvia::http2_data_submit_status::queued) {
                close();
                return;
            }
            body_offset_ += size;
        }
        flush();
        read_network();
        if (upload_pending_ && body_offset_ == body_.size() && !writing_ && !protocol_.wants_write() && !protocol_.has_queued_data(stream_id_)) {
            upload_pending_ = false;
            body_.clear();
        }
    }
    void flush() {
        if (closed_ || writing_ || !protocol_.wants_write()) {
            return;
        }
        output_.clear();
        (void)protocol_.take_output_batch(65536, output_);
        if (output_.empty()) {
            return;
        }
        writing_ = true;
        std::visit([self = shared_from_this()](auto& stream) {
            asio::async_write(stream, asio::buffer(self->output_), [self](std::error_code error, std::size_t) {
                self->writing_ = false;
                if (error) {
                    self->close();
                    return;
                }
                self->pump();
            });
        },
            stream_);
    }
    void read_network() {
        if (closed_ || reading_ || buffered_ >= 128 * 1024) {
            return;
        }
        reading_ = true;
        std::visit([self = shared_from_this()](auto& stream) {
            stream.async_read_some(asio::buffer(self->input_), [self](std::error_code error, std::size_t size) {
                self->reading_ = false;
                if (error) {
                    self->close();
                    return;
                }
                try {
                    const auto result = self->protocol_.feed(std::string_view(self->input_.data(), size));
                    if (result == ruvia::http2_feed_result::protocol_failure) {
                        self->close();
                        return;
                    }
                    self->events();
                    self->pump();
                } catch (...) {
                    self->close();
                }
            });
        },
            stream_);
    }
    std::variant<asio::ip::tcp::socket, tls_stream> stream_;
    bool secure_{};
    ruvia::http2_connection protocol_;
    std::uint32_t stream_id_{};
    std::string method_, body_;
    std::size_t body_offset_{}, write_size_{};
    std::array<char, 16384> input_{};
    std::pmr::string output_;
    std::deque<segment> queue_;
    std::size_t buffered_{};
    asio::mutable_buffer read_buffer_;
    completion read_handler_, write_handler_;
    bool reading_{}, writing_{}, closed_{}, goaway_{}, chunked_{}, upload_pending_{}, response_complete_{true};
};
}  // namespace flexedge::node
