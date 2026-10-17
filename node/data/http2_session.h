#pragma once

#include <ruvia/http/http1_client_request_writer.h>
#include <ruvia/http/http2_connection.h>

#include <charconv>
#include <cstring>
#include <functional>
#include <map>
#include <variant>

#include <asio/post.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/write.hpp>

#include "node/data/http_session.h"

namespace flexedge::node {
class http2_session;
class http2_stream_state final : public std::enable_shared_from_this<http2_stream_state> {
public:
    using completion = std::function<void(std::error_code, std::size_t)>;
    http2_stream_state(std::weak_ptr<http2_session> owner, asio::any_io_executor executor,
        std::uint32_t id, buffered_bytes_lease reservation)
        : owner_(std::move(owner)),
          executor_(std::move(executor)),
          id_(id),
          reservation_(std::move(reservation)) {}
    void read(asio::mutable_buffer, completion);
    void write(asio::const_buffer, completion);
    void close();
    void abort();
    void pump();
    std::weak_ptr<http2_session> owner_;
    asio::any_io_executor executor_;
    std::uint32_t id_;
    buffered_bytes_lease reservation_;
    std::optional<ruvia::http2_request_head_event> request_;
    std::weak_ptr<edge_connection> connection_;
    std::string method_, head_, body_, input_;
    std::size_t input_offset_{};
    std::chrono::steady_clock::time_point started_at_{std::chrono::steady_clock::now()};
    bool started_{}, closed_{}, response_head_{}, response_done_{}, chunked_{};
    std::uint64_t remaining_{};
    bool known_length_{};
    std::string response_headers_;
    std::unique_ptr<ruvia::http1_client_response_parser> parser_;
    std::optional<ruvia::http1_chunked_body_decoder> chunks_;
    asio::const_buffer output_;
    std::size_t output_offset_{};
    completion written_;
    void complete_write(std::error_code error = {}) {
        if (!written_) {
            return;
        }
        auto handler = std::move(written_);
        const auto size = error ? 0 : output_.size();
        asio::post(executor_, [handler = std::move(handler), error, size]() mutable { handler(error, size); });
    }
};

// A stream presents the same byte transport contract to the common HTTP
// request pipeline. Closing it ends that stream, never another request or socket.
class http2_stream_transport final {
public:
    using executor_type = asio::any_io_executor;
    using lowest_layer_type = http2_stream_transport;
    explicit http2_stream_transport(std::shared_ptr<http2_stream_state> state, asio::ip::tcp::endpoint peer)
        : state_(std::move(state)),
          peer_(std::move(peer)) {}
    executor_type get_executor() const {
        return state_->executor_;
    }
    lowest_layer_type& lowest_layer() {
        return *this;
    }
    asio::ip::tcp::endpoint remote_endpoint(std::error_code& error) const {
        error.clear();
        return peer_;
    }
    void shutdown(asio::socket_base::shutdown_type, std::error_code& error) {
        error.clear();
    }
    std::error_code close(std::error_code& error) {
        error.clear();
        state_->close();
        return error;
    }
    template <class buffers, class handler_type>
    void async_read_some(const buffers& buffer, handler_type&& handler) {
        auto owned = std::make_shared<std::decay_t<handler_type>>(std::forward<handler_type>(handler));
        state_->read(asio::mutable_buffer(buffer), [owned](auto error, auto size) { (*owned)(error, size); });
    }
    template <class buffers, class handler_type>
    void async_write_some(const buffers& buffer, handler_type&& handler) {
        auto owned = std::make_shared<std::decay_t<handler_type>>(std::forward<handler_type>(handler));
        auto begin = asio::buffer_sequence_begin(buffer);
        const auto end = asio::buffer_sequence_end(buffer);
        while (begin != end && asio::buffer_size(*begin) == 0) {
            ++begin;
        }
        if (begin == end) {
            asio::post(get_executor(), [owned] { (*owned)(std::error_code{}, std::size_t{}); });
            return;
        }
        // async_write advances a buffer sequence after each partial completion.
        // This stream borrows only the first nonempty segment until its callback.
        state_->write(asio::const_buffer(*begin), [owned](auto error, auto size) { (*owned)(error, size); });
    }

private:
    std::shared_ptr<http2_stream_state> state_;
    asio::ip::tcp::endpoint peer_;
};

class http2_session final : public std::enable_shared_from_this<http2_session> {
public:
    using tls_stream = asio::ssl::stream<asio::ip::tcp::socket>;
    template <class stream_type>
    http2_session(stream_type stream, const http_session_context& context, std::uint64_t sequence,
        std::shared_ptr<edge_connection> connection, bool secure, std::string fingerprint = {})
        : stream_(std::move(stream)),
          context_(context),
          sequence_(sequence),
          connection_(std::move(connection)),
          secure_(secure),
          fingerprint_(std::move(fingerprint)),
          timer_(executor()),
          protocol_(ruvia::http2_connection::server()) {
        std::error_code error;
        peer_ = std::visit([&](auto& stream) { return stream.lowest_layer().remote_endpoint(error); }, stream_);
        if (!error) {
            client_ = connected_clients::get_key(peer_.address());
            context_.clients_.opened(*client_);
        }
        context_.metrics_.connection_opened();
    }
    ~http2_session() {
        if (client_) {
            context_.clients_.closed(*client_);
        }
        context_.metrics_.connection_closed();
    }
    void start(std::string initial = {}) {
        connection_->set_close([weak = weak_from_this()] {if(auto self=weak.lock()){self->close();} });
        if (!initial.empty()) {
            if (!feed(initial)) {
                return;
            }
        }
        tick();
        read_network();
        progress();
    }
    auto executor() {
        return std::visit([](auto& stream) -> asio::any_io_executor { return stream.get_executor(); }, stream_);
    }
    void close() noexcept {
        if (closed_) {
            return;
        }
        closed_ = true;
        std::error_code ignored;
        timer_.cancel(ignored);
        std::visit([&](auto& stream) { stream.lowest_layer().close(ignored); }, stream_);
        for (auto& [id, stream] : streams_) {
            (void)id;
            stream->abort();
        }
        streams_.clear();
    }
    void retire(std::uint32_t id) {
        const auto found = streams_.find(id);
        if (found == streams_.end()) {
            return;
        }
        if (found->second->request_) {
            (void)protocol_.release(std::move(*found->second->request_));
        }
        streams_.erase(found);
        progress();
    }
    void progress() {
        if (closed_) {
            return;
        }
        for (auto& [id, stream] : streams_) {
            (void)id;
            stream->pump();
        }
        flush();
        if (!writing_ && !protocol_.wants_write()) {
            for (auto& [id, stream] : streams_) {
                if (stream->written_ && stream->output_offset_ == stream->output_.size() && !protocol_.has_queued_data(id)) {
                    stream->complete_write();
                }
            }
        }
    }
    ruvia::http2_connection& protocol() {
        return protocol_;
    }

private:
    bool feed(std::string_view bytes) {
        try {
            if (protocol_.feed(bytes) == ruvia::http2_feed_result::protocol_failure) {
                close();
                return false;
            }
            last_activity_ = std::chrono::steady_clock::now();
            while (auto event = protocol_.next_event()) {
                if (auto* head = event->request_head()) {
                    const auto id = head->stream_id();
                    const auto view = protocol_.server_request_view(id);
                    if (streams_.size() >= 64 || !view || head->request().method() == "CONNECT") {
                        (void)protocol_.submit_reset(id, ruvia::http2_error_code::refused_stream);
                        continue;
                    }
                    auto stream = std::make_shared<http2_stream_state>(weak_from_this(), executor(), id, context_.request_buffers_.lease());
                    stream->method_ = head->request().method();
                    stream->head_ = stream->method_ + " " + std::string(head->request().target()) + " HTTP/1.1\r\nHost: " + std::string(view->authority_) + "\r\n";
                    for (const auto& field : head->request().headers()) {
                        if (request_hop_by_hop_header(field.name())) {
                            continue;
                        }
                        stream->head_.append(field.name()).append(": ").append(field.value()).append("\r\n");
                    }
                    if (!stream->reservation_.try_grow(stream->head_.size())) {
                        (void)protocol_.submit_reset(id, ruvia::http2_error_code::enhance_your_calm);
                        continue;
                    }
                    const auto expect = head->request().header("Expect");
                    const bool immediate = !head->snapshot().body_open_;
                    stream->request_.emplace(std::move(*head));
                    streams_.emplace(id, stream);
                    if (expect && flexedge::route_policy::ascii_equal(*expect, "100-continue") && !immediate) {
                        (void)protocol_.submit_interim_response_head(id, ruvia::http_interim_response_head(ruvia::http_status_code::from_value(100)));
                    }
                    if (immediate) {
                        start_request(stream);
                    }
                } else if (auto* body = event->message_body_chunk()) {
                    const auto found = streams_.find(body->stream_id());
                    if (found == streams_.end()) {
                        continue;
                    }
                    auto& stream = *found->second;
                    if (stream.body_.size() + stream.head_.size() + body->bytes().size() > 2 * 1024 * 1024 ||
                        !stream.reservation_.try_grow(body->bytes().size())) {
                        (void)protocol_.submit_reset(body->stream_id(), ruvia::http2_error_code::enhance_your_calm);
                        stream.abort();
                    } else {
                        stream.body_.append(body->bytes());
                    }
                } else if (const auto* end = event->message_end()) {
                    const auto found = streams_.find(end->stream_id());
                    if (found != streams_.end()) {
                        start_request(found->second);
                    }
                } else if (const auto* reset = event->stream_closed()) {
                    const auto found = streams_.find(reset->stream_id());
                    if (found != streams_.end()) {
                        found->second->abort();
                        if (!found->second->started_) {
                            retire(reset->stream_id());
                        }
                    }
                }
            }
            return true;
        } catch (...) {
            close();
            return false;
        }
    }
    void start_request(const std::shared_ptr<http2_stream_state>& stream) {
        if (stream->started_ || stream->closed_) {
            return;
        }
        stream->started_ = true;
        stream->head_.append("Content-Length: ").append(std::to_string(stream->body_.size())).append("\r\n\r\n");
        stream->input_ = std::move(stream->head_);
        stream->input_.append(stream->body_);
        std::string().swap(stream->body_);
        const auto id = stream->id_;
        auto child = std::make_shared<edge_connection>([weak = weak_from_this(), id, retained = connection_] {(void)retained;if(auto owner=weak.lock()){owner->retire(id);} });
        child->retain_tls_context({}, std::string(connection_->tls_server_name()));
        stream->connection_ = child;
        auto session = std::make_shared<basic_http_session<http2_stream_transport>>(http2_stream_transport(stream, peer_), context_,
            ++sequence_, std::move(child), secure_, fingerprint_, false, false);
        session->start();
    }
    void read_network() {
        if (closed_ || reading_) {
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
                if (self->feed(std::string_view(self->input_.data(), size))) {
                    self->progress();
                    self->read_network();
                }
            });
        },
            stream_);
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
                self->last_activity_ = std::chrono::steady_clock::now();
                self->progress();
            });
        },
            stream_);
    }
    void tick() {
        if (closed_) {
            return;
        }
        timer_.expires_after(std::chrono::seconds(1));
        timer_.async_wait([self = shared_from_this()](std::error_code error) {
            if (error) {
                return;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now - self->last_activity_ > std::chrono::seconds(75)) {
                self->close();
                return;
            }
            std::vector<std::uint32_t> expired;
            for (auto& [id, stream] : self->streams_) {
                if (!stream->started_ && now - stream->started_at_ > std::chrono::seconds(30)) {
                    (void)self->protocol_.submit_reset(id, ruvia::http2_error_code::cancel);
                    stream->abort();
                    expired.push_back(id);
                }
            }
            for (const auto id : expired) {
                self->retire(id);
            }
            self->progress();
            self->tick();
        });
    }
    std::variant<asio::ip::tcp::socket, tls_stream> stream_;
    http_session_context context_;
    std::uint64_t sequence_;
    std::shared_ptr<edge_connection> connection_;
    bool secure_;
    std::string fingerprint_;
    asio::steady_timer timer_;
    ruvia::http2_connection protocol_;
    asio::ip::tcp::endpoint peer_;
    std::optional<connected_clients::address_key> client_;
    std::map<std::uint32_t, std::shared_ptr<http2_stream_state>> streams_;
    std::array<char, 16384> input_;
    std::pmr::string output_;
    std::chrono::steady_clock::time_point last_activity_{std::chrono::steady_clock::now()};
    bool reading_{}, writing_{}, closed_{};
};

inline void http2_stream_state::read(asio::mutable_buffer buffer, completion handler) {
    if (closed_ || input_offset_ >= input_.size()) {
        asio::post(executor_, [handler = std::move(handler)]() mutable { handler(asio::error::eof, 0); });
        return;
    }
    const auto size = (std::min)(buffer.size(), input_.size() - input_offset_);
    std::memcpy(buffer.data(), input_.data() + input_offset_, size);
    input_offset_ += size;
    reservation_.shrink(size);
    if (input_offset_ == input_.size()) {
        std::string().swap(input_);
    }
    asio::post(executor_, [handler = std::move(handler), size]() mutable { handler({}, size); });
}
inline void http2_stream_state::write(asio::const_buffer bytes, completion handler) {
    if (closed_ || written_) {
        asio::post(executor_, [handler = std::move(handler)]() mutable { handler(asio::error::operation_aborted, 0); });
        return;
    }
    output_ = bytes;
    output_offset_ = 0;
    written_ = std::move(handler);
    if (auto owner = owner_.lock()) {
        owner->progress();
    } else {
        abort();
    }
}
inline void http2_stream_state::abort() {
    if (closed_) {
        return;
    }
    closed_ = true;
    complete_write(asio::error::operation_aborted);
    if (auto owner = owner_.lock()) {
        (void)owner->protocol().submit_reset(id_, ruvia::http2_error_code::cancel);
        asio::post(executor_, [owner] { owner->progress(); });
    }
    if (auto connection = connection_.lock()) {
        connection->close();
    }
}
inline void http2_stream_state::close() {
    if (closed_) {
        return;
    }
    closed_ = true;
    if (auto owner = owner_.lock()) {
        if (response_head_ && !response_done_ && !chunked_ && !known_length_) {
            (void)owner->protocol().submit_data(id_, {}, ruvia::http2_end_stream::end_stream);
            response_done_ = true;
        } else if (!response_done_) {
            (void)owner->protocol().submit_reset(id_, ruvia::http2_error_code::cancel);
        }
        asio::post(executor_, [owner] { owner->progress(); });
    }
}
inline void http2_stream_state::pump() {
    if (closed_ || !written_) {
        return;
    }
    auto owner = owner_.lock();
    if (!owner) {
        abort();
        return;
    }
    auto& protocol = owner->protocol();
    try {
        if (!parser_) {
            std::array<char, 1024> storage{};
            ruvia::http_client_request_view request{.method_ = std::string_view(method_)};
            const auto prepared = ruvia::http1_client_request_writer{}.prepare(ruvia::http_origin_view::http({.host_ = "internal"}), request, storage);
            parser_ = std::make_unique<ruvia::http1_client_response_parser>(prepared.prepared()->exchange_state());
        }
        while (output_offset_ < output_.size() && !protocol.has_queued_data(id_)) {
            auto bytes = std::string_view(static_cast<const char*>(output_.data()) + output_offset_, output_.size() - output_offset_);
            if (!response_head_) {
                const auto marker = response_headers_.empty() ? bytes.find("\r\n\r\n") : std::string_view::npos;
                if (marker == std::string_view::npos) {
                    // Headers can span writes; append one byte at a time until complete.
                    response_headers_.push_back(bytes.front());
                    ++output_offset_;
                    if (!response_headers_.ends_with("\r\n\r\n")) {
                        if (response_headers_.size() > 1024 * 1024) {
                            abort();
                        }
                        continue;
                    }
                } else {
                    response_headers_.append(bytes.substr(0, marker + 4));
                    output_offset_ += marker + 4;
                }
                const auto parsed = parser_->parse(response_headers_);
                if (!parsed.parsed()) {
                    abort();
                    return;
                }
                const auto& head = parsed.parsed()->head();
                const auto& plan = parsed.parsed()->plan();
                if (plan.informational()) {
                    (void)protocol.submit_interim_response_head(id_, ruvia::http_interim_response_head(head.status()));
                    response_headers_.clear();
                    continue;
                }
                ruvia::http_response response;
                response.status(head.status());
                for (const auto& field : head.headers()) {
                    if (request_hop_by_hop_header(field.name()) && !flexedge::route_policy::ascii_equal(field.name(), "Content-Length")) {
                        continue;
                    }
                    response.header(field.name(), field.value());
                }
                if (protocol.submit_streaming_response_head(id_, std::move(response)) != ruvia::http2_submit_status::accepted) {
                    abort();
                    return;
                }
                response_head_ = true;
                if (plan.zero_content()) {
                    response_done_ = true;
                } else if (const auto* known = plan.known_length()) {
                    known_length_ = true;
                    remaining_ = known->content_length();
                } else if (plan.chunked()) {
                    chunked_ = true;
                    chunks_.emplace();
                }
                if (known_length_ && remaining_ == 0) {
                    (void)protocol.submit_data(id_, {}, ruvia::http2_end_stream::end_stream);
                    response_done_ = true;
                }
                response_headers_.clear();
                continue;
            }
            if (response_done_) {
                abort();
                return;
            }
            if (chunked_) {
                const auto decoded = chunks_->decode(bytes);
                if (const auto* chunk = decoded.body_chunk()) {
                    const auto sent = protocol.submit_data(id_, chunk->bytes(), ruvia::http2_end_stream::keep_open);
                    if (sent != ruvia::http2_data_submit_status::accepted && sent != ruvia::http2_data_submit_status::queued) {
                        abort();
                        return;
                    }
                    output_offset_ += chunk->consumed_bytes();
                } else if (const auto* complete = decoded.complete()) {
                    output_offset_ += complete->consumed_bytes();
                    (void)protocol.submit_data(id_, {}, ruvia::http2_end_stream::end_stream);
                    response_done_ = true;
                } else if (decoded.failure()) {
                    abort();
                    return;
                } else {
                    output_offset_ += decoded.need_more()->consumed_bytes();
                    break;
                }
            } else {
                const auto size = (std::min)(std::size_t{16384}, bytes.size());
                if (known_length_ && size > remaining_) {
                    abort();
                    return;
                }
                const bool end = known_length_ && size == remaining_;
                const auto sent = protocol.submit_data(id_, bytes.substr(0, size), end ? ruvia::http2_end_stream::end_stream : ruvia::http2_end_stream::keep_open);
                if (sent == ruvia::http2_data_submit_status::backpressured) {
                    break;
                }
                if (sent != ruvia::http2_data_submit_status::accepted && sent != ruvia::http2_data_submit_status::queued) {
                    abort();
                    return;
                }
                output_offset_ += size;
                if (known_length_) {
                    remaining_ -= size;
                }
                response_done_ = end;
            }
        }
    } catch (...) {
        abort();
    }
}
}  // namespace flexedge::node
