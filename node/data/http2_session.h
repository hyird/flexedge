#pragma once

#include <array>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <asio/buffer.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/write.hpp>

#include <ruvia/http/Http2Connection.h>
#include <ruvia/http/HttpResponse.h>

#include "node/data/buffered_origin_exchange.h"
#include "node/data/buffered_bytes_budget.h"
#include "node/data/origin_health.h"
#include "node/data/origin_selection.h"
#include "node/data/response_compression.h"
#include "node/runtime/log_buffer.h"
#include "node/runtime/runtime_metrics.h"
#include "node/runtime/runtime_state.h"

namespace flexedge::node {

class Http2Session final : public std::enable_shared_from_this<Http2Session> {
  public:
    using Stream = asio::ssl::stream<asio::ip::tcp::socket>;

    Http2Session(Stream stream, RuntimeState& runtime, OriginHealthRegistry& health,
                 RuntimeMetrics& metrics, NodeLogBuffer::Producer& logs,
                 OriginConnectionPool& originConnections, BufferedBytesBudget& requestBuffers,
                 BufferedBytesBudget& responseBuffers, std::uint64_t sequence,
                 std::shared_ptr<const void> transportState, std::string tlsFingerprint = {})
        : stream_(std::move(stream)), runtime_(runtime), health_(health), metrics_(metrics),
          logs_(logs), originConnections_(originConnections), requestBuffers_(requestBuffers),
          responseBuffers_(responseBuffers), sequence_(sequence),
          transportState_(std::move(transportState)), tlsFingerprint_(std::move(tlsFingerprint)),
          connection_(ruvia::Http2Connection::server()) {
        metrics_.connectionOpened();
    }

    ~Http2Session() { metrics_.connectionClosed(); }

    void start() {
        flush();
        read();
    }

  private:
    static constexpr std::size_t kMaxRequestBytes = 2 * 1024 * 1024;

    struct RequestState final {
        BufferedProxyRequest request;
        std::shared_ptr<const CompiledConfig> config;
        const v2::Website* website{};
        const v2::RouteRule* route{};
        std::vector<const v2::Origin*> origins;
        std::size_t originIndex{};
        std::shared_ptr<BufferedOriginExchange> exchange;
        BufferedBytesLease requestReservation;
        BufferedBytesLease responseReservation;
        std::optional<ruvia::Http2Event> lease;
        std::string clientIp;
        std::string userAgent;
        std::string referer;
        std::string responseHeaders;
        std::uint16_t statusCode{};
        std::uint64_t responseBytes{};
        std::chrono::steady_clock::time_point startedAt{std::chrono::steady_clock::now()};
        bool responseSubmitted{};
    };

    static bool normalIoClose(const std::error_code& error) noexcept {
        return error == asio::error::eof || error == asio::error::operation_aborted ||
               error == asio::ssl::error::stream_truncated;
    }

    static void appendBounded(std::string& result, std::string_view value, std::size_t maximum) {
        if (result.size() >= maximum) {
            return;
        }
        const auto available = maximum - result.size();
        result.append(value.substr(0, available));
    }

    static std::string
    requestHeaderBlock(const std::vector<std::pair<std::string, std::string>>& headers) {
        std::string result;
        for (const auto& [name, value] : headers) {
            appendBounded(result, name, NodeLogBuffer::kMaxRequestHeadersBytes);
            appendBounded(result, ": ", NodeLogBuffer::kMaxRequestHeadersBytes);
            appendBounded(result, NodeLogBuffer::requestHeaderValue(name, value),
                          NodeLogBuffer::kMaxRequestHeadersBytes);
            appendBounded(result, "\n", NodeLogBuffer::kMaxRequestHeadersBytes);
            if (result.size() >= NodeLogBuffer::kMaxRequestHeadersBytes) {
                break;
            }
        }
        return result;
    }

    static std::string
    responseHeaderBlock(const std::vector<std::pair<std::string, std::string>>& headers,
                        bool hstsEnabled) {
        std::string result;
        for (const auto& [name, value] : headers) {
            appendBounded(result, name, NodeLogBuffer::kMaxResponseHeadersBytes);
            appendBounded(result, ": ", NodeLogBuffer::kMaxResponseHeadersBytes);
            appendBounded(result, NodeLogBuffer::responseHeaderValue(name, value),
                          NodeLogBuffer::kMaxResponseHeadersBytes);
            appendBounded(result, "\n", NodeLogBuffer::kMaxResponseHeadersBytes);
            if (result.size() >= NodeLogBuffer::kMaxResponseHeadersBytes) {
                return result;
            }
        }
        if (hstsEnabled) {
            appendBounded(result, "Strict-Transport-Security: max-age=31536000\n",
                          NodeLogBuffer::kMaxResponseHeadersBytes);
        }
        return result;
    }

    static std::string queryStringForLog(std::string_view target) {
        const auto question = target.find('?');
        if (question == std::string_view::npos) {
            return {};
        }
        const auto fragment = target.find('#', question + 1);
        return std::string(target.substr(question + 1, fragment == std::string_view::npos
                                                           ? std::string_view::npos
                                                           : fragment - question - 1));
    }

    static std::string
    cookiesForLog(const std::vector<std::pair<std::string, std::string>>& headers) {
        std::string result;
        for (const auto& [name, value] : headers) {
            if (!httpHeaderName(name, "Cookie")) {
                continue;
            }
            if (!result.empty()) {
                appendBounded(result, "; ", NodeLogBuffer::kMaxCookiesBytes);
            }
            appendBounded(result, NodeLogBuffer::cookieValue(value),
                          NodeLogBuffer::kMaxCookiesBytes);
        }
        return result;
    }

    static bool accessLogStatusCodeEnabled(const v2::Website& website,
                                           std::uint32_t statusCode) noexcept {
        if (statusCode == 499) {
            return website.access_log_client_abort();
        }
        const auto range = statusCode / 100;
        if (range < 1 || range > 5) {
            return false;
        }
        if (website.access_log_status_code_ranges().empty()) {
            return true;
        }
        const auto wanted = std::to_string(range) + "xx";
        return std::ranges::any_of(website.access_log_status_code_ranges(),
                                   [&](const auto& value) { return value == wanted; });
    }

    void log(std::string_view message) const noexcept {
        try {
            std::cerr << "flexedge node http2: " << message << '\n';
            logs_.node("warning", "http2", message);
        } catch (...) {
        }
    }

    void log(std::string_view message, const std::exception& error) const noexcept {
        try {
            const auto text = std::string(message) + ": " + error.what();
            std::cerr << "flexedge node http2: " << text << '\n';
            logs_.node("error", "http2", text);
        } catch (...) {
        }
    }

    void logIo(std::string_view message, const std::error_code& error) const noexcept {
        if (normalIoClose(error)) {
            return;
        }
        try {
            const auto text = std::string(message) + ": " + error.message();
            std::cerr << "flexedge node http2: " << text << '\n';
            logs_.node("error", "http2", text);
        } catch (...) {
        }
    }

    std::string clientIp() noexcept {
        std::error_code endpointError;
        auto endpoint = stream_.lowest_layer().remote_endpoint(endpointError);
        if (endpointError) {
            return "unknown";
        }
        std::error_code addressError;
        auto value = endpoint.address().to_string(addressError);
        return addressError ? std::string("unknown") : value;
    }

    void close() noexcept {
        if (closed_) {
            return;
        }
        closed_ = true;
        for (auto& [_, state] : requests_) {
            if (state.exchange) {
                state.exchange->close();
            }
        }
        std::error_code ignored;
        ignored = stream_.lowest_layer().close(ignored);
    }

    void read() {
        if (closed_ || reading_) {
            return;
        }
        reading_ = true;
        const auto self = shared_from_this();
        stream_.async_read_some(asio::buffer(readBuffer_),
                                [self](const std::error_code& error, std::size_t size) {
                                    self->readCompleted(error, size);
                                });
    }

    void readCompleted(const std::error_code& error, std::size_t size) noexcept {
        reading_ = false;
        if (error) {
            logIo("client read failed", error);
            close();
            return;
        }
        try {
            const auto result = connection_.feed(std::string_view(readBuffer_.data(), size));
            if (result == ruvia::Http2FeedResult::kProtocolFailure) {
                log("client protocol failure");
                close();
                return;
            }
            drainEvents();
            flush();
            read();
        } catch (const std::exception& error) {
            log("client input handling failed", error);
            close();
        } catch (...) {
            log("client input handling failed with unknown exception");
            close();
        }
    }

    void handleRequestHead(ruvia::Http2Event& event, const ruvia::Http2RequestHeadEvent& head) {
        RequestState state;
        state.request.method = head.request().method();
        state.request.target = head.request().target();
        state.request.authority = head.request().authority();
        for (const auto& header : head.request().headers()) {
            state.request.headers.emplace_back(header.name(), header.value());
        }
        state.clientIp = clientIp();
        if (const auto userAgent = head.request().header("User-Agent")) {
            state.userAgent.assign(*userAgent);
        }
        if (const auto referer = head.request().header("Referer")) {
            state.referer.assign(*referer);
        }
        const auto id = head.streamId();
        state.requestReservation = requestBuffers_.lease();
        state.lease.emplace(std::move(event));
        requests_.emplace(id, std::move(state));
    }

    void handleMessageBodyChunk(ruvia::Http2MessageBodyChunkEvent& chunk) {
        const auto found = requests_.find(chunk.streamId());
        if (found != requests_.end()) {
            found->second.request.hasBody = true;
            if (found->second.request.body.size() + chunk.bytes().size() > kMaxRequestBytes) {
                (void)connection_.submitReset(chunk.streamId(), ruvia::Http2ErrorCode::kCancel);
                log("request body exceeded limit; stream reset");
                requests_.erase(found);
            } else if (!found->second.requestReservation.tryGrow(chunk.bytes().size())) {
                (void)connection_.submitReset(chunk.streamId(), ruvia::Http2ErrorCode::kCancel);
                log("request buffer budget exhausted; stream reset");
                requests_.erase(found);
            } else {
                found->second.request.body.append(chunk.bytes());
            }
        }
        (void)connection_.acknowledge(chunk.takeCredit());
    }

    void handleStreamClosed(const ruvia::Http2StreamClosedEvent& closed) {
        const auto found = requests_.find(closed.streamId());
        if (found != requests_.end()) {
            log("client closed stream before response completed");
            completeResponse(closed.streamId(), 499);
        }
    }

    void drainEvents() {
        while (auto event = connection_.nextEvent()) {
            if (auto* head = event->requestHead()) {
                handleRequestHead(*event, *head);
            } else if (auto* chunk = event->messageBodyChunk()) {
                handleMessageBodyChunk(*chunk);
            } else if (const auto* end = event->messageEnd()) {
                dispatch(end->streamId());
            } else if (const auto* closed = event->streamClosed()) {
                handleStreamClosed(*closed);
            } else if (event->goaway()) {
                connection_.beginDrain();
            }
        }
    }

    void dispatch(std::uint32_t streamId) {
        const auto found = requests_.find(streamId);
        if (found == requests_.end()) {
            return;
        }
        auto& state = found->second;
        state.config = runtime_.config();
        if (!state.config) {
            respond(streamId, 503);
            return;
        }
        state.website = state.config->website(state.request.authority);
        if (state.website == nullptr || !state.website->https_enabled() ||
            !state.website->http2_enabled()) {
            respond(streamId, 421);
            return;
        }
        state.route = matchedRouteRule(*state.website, state.request.method, state.request.target);
        if (state.route != nullptr && state.route->action() == "redirect") {
            BufferedProxyResponse response;
            response.status = static_cast<std::uint16_t>(state.route->redirect_status());
            response.headers.emplace_back(
                "Location", routeRedirectLocation(state.request.target, *state.route));
            for (const auto& header : state.route->response_headers()) {
                response.headers.emplace_back(header.name(), header.value());
            }
            respond(streamId, std::move(response));
            return;
        }
        state.origins = originCandidates(*state.website, sequence_ + streamId, state.route);
        std::stable_partition(state.origins.begin(), state.origins.end(), [&](const auto* origin) {
            return health_.healthy(state.website->id(), origin->id());
        });
        tryOrigin(streamId);
    }

    void tryOrigin(std::uint32_t streamId) {
        const auto found = requests_.find(streamId);
        if (found == requests_.end()) {
            return;
        }
        auto& state = found->second;
        if (state.originIndex >= state.origins.size()) {
            respond(streamId, 502);
            return;
        }
        const auto* origin = state.origins[state.originIndex++];
        std::vector<ruvia::HttpHeaderView> views;
        views.reserve(state.request.headers.size());
        for (const auto& [name, value] : state.request.headers) {
            views.emplace_back(name, value);
        }
        std::error_code endpointError;
        const auto clientAddress = stream_.lowest_layer()
                                       .remote_endpoint(endpointError)
                                       .address()
                                       .to_string(endpointError);
        auto prepared = prepareOriginRequest({.method = state.request.method,
                                              .target = state.request.target,
                                              .headers = views,
                                              .body = state.request.body,
                                              .hasBody = state.request.hasBody},
                                             *state.website, state.request.authority,
                                             endpointError ? "unknown" : clientAddress, true, false,
                                             false, state.route);
        if (!prepared) {
            respond(streamId, 502);
            return;
        }
        const auto self = shared_from_this();
        state.exchange = std::make_shared<BufferedOriginExchange>(
            stream_.get_executor(), originConnections_, std::move(prepared->bytes),
            std::move(prepared->responseExchange), origin->host(),
            static_cast<std::uint16_t>(origin->port()), origin->protocol() == "https",
            std::chrono::seconds(state.website->origin_connect_timeout_seconds() +
                                 state.website->origin_read_timeout_seconds()),
            responseBuffers_,
            [self, streamId, originId = origin->id(), originHost = origin->host(),
             originPort = origin->port()](std::error_code error,
                                          BufferedProxyResponse response) mutable {
                const auto current = self->requests_.find(streamId);
                if (current == self->requests_.end()) {
                    return;
                }
                auto& request = current->second;
                request.exchange.reset();
                if (error) {
                    if (bufferedBytesExhausted(error)) {
                        self->respond(streamId, 503);
                        return;
                    }
                    self->log(std::string("origin exchange failed for ") + originHost + ":" +
                              std::to_string(originPort) + ": " + error.message());
                    self->health_.failure(request.website->id(), originId,
                                          request.website->unhealthy_threshold());
                    self->tryOrigin(streamId);
                    return;
                }
                self->health_.success(request.website->id(), originId,
                                      request.website->healthy_threshold());
                self->respond(streamId, std::move(response));
            });
        state.exchange->start();
    }

    void respond(std::uint32_t streamId, std::uint16_t status) {
        BufferedProxyResponse response;
        response.status = status;
        respond(streamId, std::move(response));
    }

    void respond(std::uint32_t streamId, BufferedProxyResponse source) noexcept {
        try {
            respondChecked(streamId, std::move(source));
        } catch (const std::exception& error) {
            log("response handling failed", error);
            close();
        } catch (...) {
            log("response handling failed with unknown exception");
            close();
        }
    }

    static bool shouldForwardOriginResponseHeader(std::string_view name) noexcept {
        return !httpHeaderName(name, "Content-Length") &&
               !httpHeaderName(name, "Transfer-Encoding") && !httpHeaderName(name, "Connection") &&
               !httpHeaderName(name, "Upgrade");
    }

    void appendOriginResponseHeader(ruvia::HttpResponse& target, std::string_view name,
                                    std::string_view value) {
        try {
            target.header(name, value, {.mode = ruvia::HttpResponseHeaderMode::kAppend});
        } catch (const std::invalid_argument&) {
            try {
                target.header(name, value);
            } catch (const std::invalid_argument& error) {
                log(std::string("dropped invalid origin response header '") + std::string(name) +
                    "': " + error.what());
                // The origin parser protects the wire grammar, while HttpResponse also enforces
                // field-specific semantics. Ignore fields that cannot be safely represented.
            }
        }
    }

    void appendOriginResponseHeaders(std::uint32_t streamId, ruvia::HttpResponse& target,
                                     const BufferedProxyResponse& source) {
        for (const auto& [name, value] : source.headers) {
            if (shouldForwardOriginResponseHeader(name)) {
                appendOriginResponseHeader(target, name, value);
            }
        }
        const auto found = requests_.find(streamId);
        if (found != requests_.end() && found->second.website != nullptr &&
            found->second.website->hsts_enabled()) {
            target.header("Strict-Transport-Security", "max-age=31536000");
        }
    }

    void captureResponseMetadata(std::uint32_t streamId, const BufferedProxyResponse& response) {
        const auto found = requests_.find(streamId);
        if (found == requests_.end()) {
            return;
        }
        found->second.statusCode = response.status;
        found->second.responseBytes = response.body.size();
        if (found->second.website != nullptr &&
            found->second.website->access_log_response_headers()) {
            found->second.responseHeaders =
                responseHeaderBlock(response.headers, found->second.website->hsts_enabled());
        }
    }

    bool prepareResponseBody(std::uint32_t streamId, BufferedProxyResponse& source,
                             ruvia::HttpResponse& target) {
        const auto bodySize = source.body.size();
        const auto sourceReservationBytes = source.reservation.bytes();
        if (!source.reservation.tryGrow(bodySize)) {
            return false;
        }
        target.body(source.body);
        source.body.clear();
        source.body.shrink_to_fit();
        source.reservation.shrink(sourceReservationBytes);
        if (const auto found = requests_.find(streamId); found != requests_.end()) {
            found->second.responseReservation = std::move(source.reservation);
        }
        return true;
    }

    void respondChecked(std::uint32_t streamId, BufferedProxyResponse source) {
        if (const auto found = requests_.find(streamId);
            found != requests_.end() && found->second.website != nullptr) {
            flexedge::node::applyResponseCompression(found->second.request, *found->second.website,
                                                     source);
            if (found->second.route != nullptr) {
                applyRouteHeaders(source.headers, found->second.route->response_headers());
            }
        }
        captureResponseMetadata(streamId, source);
        ruvia::HttpResponse response;
        response.status(ruvia::HttpStatusCode::fromValue(source.status));
        appendOriginResponseHeaders(streamId, response, source);
        if (!prepareResponseBody(streamId, source, response)) {
            respond(streamId, 503);
            return;
        }
        const auto submitted = connection_.submitBufferedResponse(streamId, response);
        if (submitted != ruvia::Http2SubmitStatus::kAccepted) {
            log(std::string("response submit failed with status ") +
                std::to_string(static_cast<int>(submitted)));
            close();
            return;
        }
        if (connection_.hasQueuedData(streamId)) {
            if (const auto found = requests_.find(streamId); found != requests_.end()) {
                found->second.responseSubmitted = true;
            }
        } else {
            completeResponse(streamId);
        }
        flush();
    }

    void flush() {
        if (closed_ || writing_ || connection_.pendingOutput().empty()) {
            return;
        }
        writing_ = true;
        output_.assign(connection_.pendingOutput());
        const auto self = shared_from_this();
        asio::async_write(stream_, asio::buffer(output_),
                          [self](const std::error_code& error, std::size_t size) {
                              self->writeCompleted(error, size);
                          });
    }

    void writeCompleted(const std::error_code& error, std::size_t size) noexcept {
        writing_ = false;
        try {
            if (error) {
                logIo("client write failed", error);
                close();
                return;
            }
            if (connection_.consumeOutput(size) == ruvia::Http2OutputConsumeStatus::kOutOfRange) {
                log("output consume failed");
                close();
                return;
            }
            std::vector<std::uint32_t> drained;
            for (const auto streamId : connection_.takeDrainedDataStreams()) {
                drained.push_back(streamId);
            }
            for (const auto streamId : drained) {
                const auto found = requests_.find(streamId);
                if (found != requests_.end() && found->second.responseSubmitted) {
                    completeResponse(streamId);
                }
            }
            metrics_.trafficOut(size);
            flush();
        } catch (const std::exception& error) {
            log("client output handling failed", error);
            close();
        } catch (...) {
            log("client output handling failed with unknown exception");
            close();
        }
    }

    void completeResponse(std::uint32_t streamId, std::uint16_t overrideStatus = 0) {
        const auto found = requests_.find(streamId);
        if (found == requests_.end()) {
            return;
        }
        auto& lease = found->second.lease;
        if (lease) {
            auto* head = lease->requestHead();
            if (head != nullptr) {
                (void)connection_.release(std::move(*head));
            }
        }
        if (found->second.website != nullptr && found->second.website->access_log_enabled() &&
            (overrideStatus != 0 || found->second.statusCode != 0) &&
            accessLogStatusCodeEnabled(*found->second.website, overrideStatus == 0
                                                                   ? found->second.statusCode
                                                                   : overrideStatus)) {
            const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - found->second.startedAt);
            const auto durationMs = duration.count() < 0
                                        ? std::uint64_t{}
                                        : static_cast<std::uint64_t>(duration.count());
            logs_.access({
                .websiteId = found->second.website->id(),
                .clientIp = found->second.clientIp,
                .protocol = "h2",
                .method = found->second.request.method,
                .host = found->second.request.authority,
                .target = found->second.request.target,
                .statusCode = overrideStatus == 0 ? found->second.statusCode : overrideStatus,
                .responseBytes = found->second.responseBytes,
                .durationMs = durationMs,
                .userAgent = found->second.website->access_log_user_agent()
                                 ? found->second.userAgent
                                 : std::string{},
                .referer = found->second.website->access_log_referer() ? found->second.referer
                                                                       : std::string{},
                .requestHeaders = found->second.website->access_log_request_headers()
                                      ? requestHeaderBlock(found->second.request.headers)
                                      : std::string{},
                .requestBody = found->second.website->access_log_request_body()
                                   ? NodeLogBuffer::requestBodyValue(found->second.request.body)
                                   : std::string{},
                .requestBodyTruncated =
                    found->second.website->access_log_request_body() &&
                    found->second.request.body.size() > NodeLogBuffer::kMaxRequestBodyBytes,
                .tlsFingerprint = tlsFingerprint_,
                .responseHeaders = found->second.responseHeaders,
                .queryString = found->second.website->access_log_query_params()
                                   ? queryStringForLog(found->second.request.target)
                                   : std::string{},
                .cookies = found->second.website->access_log_cookies()
                               ? cookiesForLog(found->second.request.headers)
                               : std::string{},
            });
        }
        requests_.erase(found);
    }

    Stream stream_;
    RuntimeState& runtime_;
    OriginHealthRegistry& health_;
    RuntimeMetrics& metrics_;
    NodeLogBuffer::Producer& logs_;
    OriginConnectionPool& originConnections_;
    BufferedBytesBudget& requestBuffers_;
    BufferedBytesBudget& responseBuffers_;
    std::uint64_t sequence_{};
    std::shared_ptr<const void> transportState_;
    std::string tlsFingerprint_;
    ruvia::Http2Connection connection_;
    std::unordered_map<std::uint32_t, RequestState> requests_;
    std::array<char, 16384> readBuffer_{};
    std::string output_;
    bool reading_{};
    bool writing_{};
    bool closed_{};
};

} // namespace flexedge::node
