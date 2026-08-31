#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <asio/bind_executor.hpp>
#include <asio/buffer.hpp>
#include <asio/connect.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl/error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/write.hpp>

#include <ruvia/core/EventLoop.h>
#include <ruvia/http/Http1RequestParser.h>

#if defined(SO_REUSEPORT) && !defined(_WIN32)
#include <sys/socket.h>
#endif

#include "node/runtime/http_redirect.h"
#include "node/runtime/log_buffer.h"
#include "node/runtime/runtime_metrics.h"
#include "node/runtime/runtime_state.h"
#include "node/data/origin_health.h"
#include "node/data/buffered_origin_exchange.h"
#include "node/data/buffered_bytes_budget.h"
#include "node/data/origin_connection_pool.h"
#include "node/data/origin_request_codec.h"
#include "node/data/origin_response_codec.h"
#include "node/data/origin_selection.h"
#include "node/data/origin_tls.h"
#include "node/data/origin_transport.h"
#include "node/data/response_compression.h"

namespace flexedge::node {

class ListenerActivationGate final {
  public:
    void activate() noexcept { active_.store(true, std::memory_order_release); }

    [[nodiscard]] bool active() const noexcept { return active_.load(std::memory_order_acquire); }

  private:
    std::atomic<bool> active_{};
};

template <typename ClientStream>
class BasicHttpSession final : public std::enable_shared_from_this<BasicHttpSession<ClientStream>> {
  public:
    BasicHttpSession(ClientStream stream, RuntimeState& runtime, OriginHealthRegistry& health,
                     RuntimeMetrics& metrics, NodeLogBuffer::Producer& logs,
                     OriginConnectionPool& originConnections, BufferedBytesBudget& requestBuffers,
                     BufferedBytesBudget& responseBuffers, std::uint64_t sequence,
                     std::shared_ptr<const void> transportState = nullptr, bool secure = false,
                     std::string tlsFingerprint = {})
        : stream_(std::move(stream)), originConnections_(originConnections),
          timer_(stream_.get_executor()), runtime_(runtime), health_(health), metrics_(metrics),
          logs_(logs), responseBuffers_(responseBuffers),
          requestReservation_(requestBuffers.lease()), tlsFingerprint_(std::move(tlsFingerprint)),
          sequence_(sequence), transportState_(std::move(transportState)), secure_(secure) {
        requestBytes_.reserve(8192);
        metrics_.connectionOpened();
    }

    ~BasicHttpSession() { metrics_.connectionClosed(); }

    void start() {
        captureClientAddress();
        read();
    }

  private:
    static constexpr std::size_t kMaxRequestBytes = 2 * 1024 * 1024;

    enum class TimeoutAction { kClose, kFailover };

    static std::string response(std::string_view status, std::string_view extraHeaders = {}) {
        std::string result = "HTTP/1.1 ";
        result.append(status);
        result.append("\r\nContent-Length: 0\r\nConnection: close\r\n");
        result.append(extraHeaders);
        result.append("\r\n");
        return result;
    }

    static bool shouldForwardBufferedResponseHeader(std::string_view name,
                                                    bool hstsEnabled) noexcept {
        return !httpHeaderName(name, "Content-Length") &&
               !httpHeaderName(name, "Transfer-Encoding") && !httpHeaderName(name, "Connection") &&
               !(hstsEnabled && httpHeaderName(name, "Strict-Transport-Security"));
    }

    static bool normalIoClose(const std::error_code& error) noexcept {
        return error == asio::error::eof || error == asio::error::operation_aborted ||
               error == asio::ssl::error::stream_truncated;
    }

    void log(std::string_view message) const noexcept {
        try {
            std::cerr << "flexedge node http1: " << message << '\n';
            logs_.node("warning", "http1", message);
        } catch (...) {
        }
    }

    void log(std::string_view message, const std::exception& error) const noexcept {
        try {
            const auto text = std::string(message) + ": " + error.what();
            std::cerr << "flexedge node http1: " << text << '\n';
            logs_.node("error", "http1", text);
        } catch (...) {
        }
    }

    void logIo(std::string_view message, const std::error_code& error) const noexcept {
        if (normalIoClose(error)) {
            return;
        }
        try {
            const auto text = std::string(message) + ": " + error.message();
            std::cerr << "flexedge node http1: " << text << '\n';
            logs_.node("error", "http1", text);
        } catch (...) {
        }
    }

    static std::uint32_t wireStatusCode(std::string_view bytes) noexcept {
        if (!bytes.starts_with("HTTP/1.1 ") || bytes.size() < 12) {
            return 0;
        }
        const auto a = bytes[9];
        const auto b = bytes[10];
        const auto c = bytes[11];
        if (!std::isdigit(static_cast<unsigned char>(a)) ||
            !std::isdigit(static_cast<unsigned char>(b)) ||
            !std::isdigit(static_cast<unsigned char>(c))) {
            return 0;
        }
        return static_cast<std::uint32_t>((a - '0') * 100 + (b - '0') * 10 + (c - '0'));
    }

    static void appendBounded(std::string& result, std::string_view value, std::size_t maximum) {
        if (result.size() >= maximum) {
            return;
        }
        const auto available = maximum - result.size();
        result.append(value.substr(0, available));
    }

    static std::string requestHeaderBlock(const ruvia::Http1ParsedRequest& parsed) {
        std::string result;
        for (const auto& header : parsed.request().headers()) {
            appendBounded(result, header.name(), NodeLogBuffer::kMaxRequestHeadersBytes);
            appendBounded(result, ": ", NodeLogBuffer::kMaxRequestHeadersBytes);
            appendBounded(result, NodeLogBuffer::requestHeaderValue(header.name(), header.value()),
                          NodeLogBuffer::kMaxRequestHeadersBytes);
            appendBounded(result, "\n", NodeLogBuffer::kMaxRequestHeadersBytes);
            if (result.size() >= NodeLogBuffer::kMaxRequestHeadersBytes) {
                break;
            }
        }
        return result;
    }

    static std::string headerBlockForLog(std::string_view block, std::size_t maximum) {
        std::string result;
        while (!block.empty() && result.size() < maximum) {
            const auto lineEnd = block.find("\r\n");
            const auto line = lineEnd == std::string_view::npos ? block : block.substr(0, lineEnd);
            const auto colon = line.find(':');
            if (colon == std::string_view::npos) {
                appendBounded(result, line, maximum);
            } else {
                auto value = line.substr(colon + 1);
                while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
                    value.remove_prefix(1);
                }
                const auto name = line.substr(0, colon);
                appendBounded(result, name, maximum);
                appendBounded(result, ": ", maximum);
                appendBounded(result, NodeLogBuffer::responseHeaderValue(name, value), maximum);
            }
            appendBounded(result, "\n", maximum);
            if (lineEnd == std::string_view::npos) {
                break;
            }
            block.remove_prefix(lineEnd + 2);
        }
        return result;
    }

    static std::pair<std::string, bool> requestBodyForLog(const ruvia::Http1ParsedRequest& parsed) {
        std::string body;
        if (parsed.bodyPlan().chunked()) {
            auto decoded = decodeChunkedBody(parsed.wireBody(), kMaxRequestBytes);
            if (!decoded) {
                return {};
            }
            body = std::move(*decoded);
        } else if (parsed.bodyPlan().knownLength()) {
            body.assign(parsed.wireBody());
        }
        const auto truncated = body.size() > NodeLogBuffer::kMaxRequestBodyBytes;
        if (truncated) {
            body.resize(NodeLogBuffer::kMaxRequestBodyBytes);
        }
        return {NodeLogBuffer::requestBodyValue(body), truncated};
    }

    static std::string_view http1ResponseHeaders(std::string_view bytes) noexcept {
        const auto headEnd = bytes.find("\r\n\r\n");
        const auto statusLineEnd = bytes.find("\r\n");
        if (headEnd == std::string_view::npos || statusLineEnd == std::string_view::npos ||
            statusLineEnd + 2 > headEnd) {
            return {};
        }
        return bytes.substr(statusLineEnd + 2, headEnd - statusLineEnd - 2);
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

    static void applyStatusCodeRanges(std::array<bool, 5>& target, const v2::Website& website) {
        if (website.access_log_status_code_ranges().empty()) {
            target.fill(true);
            return;
        }
        target.fill(false);
        for (const auto& value : website.access_log_status_code_ranges()) {
            if (value == "1xx") {
                target[0] = true;
            } else if (value == "2xx") {
                target[1] = true;
            } else if (value == "3xx") {
                target[2] = true;
            } else if (value == "4xx") {
                target[3] = true;
            } else if (value == "5xx") {
                target[4] = true;
            }
        }
    }

    void captureRequestLog(const ruvia::Http1ParsedRequest& parsed, const v2::Website& website) {
        if (website.access_log_query_params()) {
            queryString_ = queryStringForLog(parsed.request().target());
        }
        if (website.access_log_cookies()) {
            if (const auto cookies = parsed.request().header("Cookie")) {
                cookies_ = NodeLogBuffer::cookieValue(*cookies);
            }
        }
        if (website.access_log_referer()) {
            if (const auto referer = parsed.request().header("Referer")) {
                requestReferer_.assign(*referer);
            }
        }
        if (website.access_log_user_agent()) {
            if (const auto userAgent = parsed.request().header("User-Agent")) {
                requestUserAgent_.assign(*userAgent);
            }
        }
        if (website.access_log_request_headers()) {
            requestHeaders_ = requestHeaderBlock(parsed);
        }
        if (website.access_log_request_body()) {
            auto [body, truncated] = requestBodyForLog(parsed);
            requestBody_ = std::move(body);
            requestBodyTruncated_ = truncated;
        }
    }

    void captureResponseHeaders(std::string_view bytes) {
        if (!accessLogResponseHeadersEnabled_ || !responseHeaders_.empty()) {
            return;
        }
        responseHeaders_ =
            headerBlockForLog(http1ResponseHeaders(bytes), NodeLogBuffer::kMaxResponseHeadersBytes);
    }

    bool accessLogStatusCodeEnabled(std::uint32_t statusCode) const noexcept {
        if (statusCode == 499) {
            return accessLogClientAbortEnabled_;
        }
        const auto range = statusCode / 100;
        return range >= 1 && range <= 5 && accessLogStatusCodeRanges_[range - 1];
    }

    bool captureClientAddress() noexcept {
        if (!clientAddress_.empty()) {
            return clientAddress_ != "unknown";
        }
        std::error_code endpointError;
        const auto endpoint = clientSocket().remote_endpoint(endpointError);
        if (endpointError) {
            clientAddress_ = "unknown";
            return false;
        }
        std::error_code addressError;
        clientAddress_ = endpoint.address().to_string(addressError);
        if (addressError) {
            clientAddress_ = "unknown";
            return false;
        }
        return true;
    }

    std::string clientIp() const noexcept {
        return clientAddress_.empty() ? std::string("unknown") : clientAddress_;
    }

    void recordAccess(std::uint32_t statusCode, std::uint64_t responseBytes) noexcept {
        if (accessRecorded_ || !accessLogEnabled_ || websiteId_.empty() || statusCode == 0) {
            return;
        }
        if (!accessLogStatusCodeEnabled(statusCode)) {
            return;
        }
        accessRecorded_ = true;
        const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - requestStartedAt_);
        const auto durationMs =
            duration.count() < 0 ? std::uint64_t{} : static_cast<std::uint64_t>(duration.count());
        logs_.access({
            .websiteId = websiteId_,
            .clientIp = clientIp(),
            .protocol = secure_ ? "https" : "http",
            .method = requestMethod_,
            .host = requestHost_,
            .target = requestTarget_,
            .statusCode = statusCode,
            .responseBytes = responseBytes,
            .durationMs = durationMs,
            .userAgent = requestUserAgent_,
            .referer = requestReferer_,
            .requestHeaders = requestHeaders_,
            .requestBody = requestBody_,
            .requestBodyTruncated = requestBodyTruncated_,
            .tlsFingerprint = tlsFingerprint_,
            .responseHeaders = responseHeaders_,
            .queryString = queryString_,
            .cookies = cookies_,
        });
    }

    void close() noexcept {
        if (closed_) {
            return;
        }
        closed_ = true;
        ++originAttempt_;
        std::error_code ignored;
        timer_.cancel(ignored);
        discardOriginTransport();
        if (bufferedExchange_) {
            bufferedExchange_->close();
            bufferedExchange_.reset();
        }
        clientSocket().shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
        ignored = clientSocket().close(ignored);
    }

    void send(std::string bytes, BufferedBytesLease reservation = {}) {
        if (closed_ || responsePending_) {
            return;
        }
        responsePending_ = true;
        cancelTimer();
        discardOriginTransport();
        if (bufferedExchange_) {
            bufferedExchange_->close();
            bufferedExchange_.reset();
        }
        captureResponseHeaders(bytes);
        responseReservation_ = std::move(reservation);
        responseBytes_ = std::move(bytes);
        const auto self = this->shared_from_this();
        asio::async_write(stream_, asio::buffer(responseBytes_),
                          [self](const std::error_code& error, std::size_t size) {
                              self->metrics_.trafficOut(size);
                              self->recordAccess(error && self->accessLogClientAbortEnabled_
                                                     ? 499
                                                     : wireStatusCode(self->responseBytes_),
                                                 size);
                              if (error) {
                                  self->logIo("client response write failed", error);
                              }
                              self->close();
                          });
    }

    void handleRequest(const ruvia::Http1ParsedRequest& parsed) {
        const auto config = runtime_.config();
        if (!config) {
            send(response("503 Service Unavailable"));
            return;
        }
        const auto host = parsed.request().header("Host");
        if (!host) {
            send(response("400 Bad Request"));
            return;
        }
        requestMethod_.assign(parsed.request().method());
        requestTarget_.assign(parsed.request().target());
        requestHost_.assign(*host);
        const auto* website = config->website(*host);
        if (website != nullptr) {
            websiteId_ = website->id();
            accessLogEnabled_ = website->access_log_enabled();
            if (accessLogEnabled_) {
                accessLogResponseHeadersEnabled_ = website->access_log_response_headers();
                accessLogClientAbortEnabled_ = website->access_log_client_abort();
                applyStatusCodeRanges(accessLogStatusCodeRanges_, *website);
                captureRequestLog(parsed, *website);
            }
        }
        if (!secure_) {
            const auto redirect = httpToHttpsRedirect(*config, *host, parsed.request().target());
            if (redirect) {
                send(response("301 Moved Permanently", "Location: " + redirect->location + "\r\n"));
                return;
            }
        }
        if (website == nullptr) {
            send(response("421 Misdirected Request"));
            return;
        }
        hstsEnabled_ = secure_ && website->hsts_enabled();
        const auto* route =
            matchedRouteRule(*website, parsed.request().method(), parsed.request().target());
        if (route != nullptr && route->action() == "redirect") {
            std::string headers =
                "Location: " + routeRedirectLocation(parsed.request().target(), *route) + "\r\n";
            for (const auto& header : route->response_headers()) {
                headers.append(header.name()).append(": ").append(header.value()).append("\r\n");
            }
            send(response(route->redirect_status() == 301 ? "301 Moved Permanently" : "302 Found",
                          headers));
            return;
        }
        upgradeRequested_ = parsed.request().header("Upgrade").has_value();
        const auto consumedBytes = parsed.consumedBytes();
        if (upgradeRequested_ && consumedBytes < requestBytes_.size()) {
            clientUpgradeRemainder_.assign(requestBytes_.data() + consumedBytes,
                                           requestBytes_.size() - consumedBytes);
        }
        activeConfig_ = config;
        origins_ = originCandidates(*website, sequence_, route);
        std::stable_partition(origins_.begin(), origins_.end(), [&](const v2::Origin* origin) {
            return health_.healthy(website->id(), origin->id());
        });
        websiteId_ = website->id();
        healthyThreshold_ = website->healthy_threshold();
        unhealthyThreshold_ = website->unhealthy_threshold();
        if (origins_.empty()) {
            send(response("502 Bad Gateway"));
            return;
        }
        proxy(parsed, *website, *host, route);
    }

    void arm(std::chrono::seconds timeout, std::uint64_t attempt,
             TimeoutAction action = TimeoutAction::kClose) {
        timer_.expires_after(timeout);
        const auto self = this->shared_from_this();
        timer_.async_wait([self, attempt, action](const std::error_code& error) {
            if (error || !self->originAttemptCurrent(attempt)) {
                return;
            }
            self->log("origin attempt timed out");
            self->health_.failure(self->websiteId_, self->currentOriginId_,
                                  self->unhealthyThreshold_);
            if (action == TimeoutAction::kFailover) {
                self->tryNextOrigin();
            } else {
                self->close();
            }
        });
    }

    void cancelTimer() noexcept {
        std::error_code ignored;
        timer_.cancel(ignored);
    }

    void discardOriginTransport() noexcept {
        if (originTransport_) {
            originTransport_->close();
        }
        originTransport_.reset();
        originConnectionKey_.reset();
        reusedOriginTransport_ = false;
    }

    void releaseOriginTransport() noexcept {
        if (originTransport_ && originConnectionKey_ && responseCodec_ &&
            responseCodec_->originReusable()) {
            originConnections_.release(std::move(*originConnectionKey_),
                                       std::move(originTransport_));
        } else {
            discardOriginTransport();
            return;
        }
        originConnectionKey_.reset();
        reusedOriginTransport_ = false;
    }

    bool retryStaleOriginTransport() {
        if (!reusedOriginTransport_ || retriedReusedOriginTransport_ || originIndex_ == 0) {
            return false;
        }
        retriedReusedOriginTransport_ = true;
        discardOriginTransport();
        --originIndex_;
        tryNextOrigin();
        return true;
    }

    [[nodiscard]] bool originAttemptCurrent(std::uint64_t attempt) const noexcept {
        return !closed_ && !responsePending_ && attempt == originAttempt_;
    }

    bool prepareOriginRequest(const ruvia::Http1ParsedRequest& parsed, const v2::Website& website,
                              std::string_view incomingHost, bool forceIdentityEncoding) {
        if (!captureClientAddress()) {
            return false;
        }
        auto prepared = flexedge::node::prepareOriginRequest(
            parsed, website, incomingHost, clientAddress_, secure_, upgradeRequested_, false,
            kMaxRequestBytes, forceIdentityEncoding, activeRoute_);
        if (!prepared) {
            return false;
        }
        outboundRequest_ = std::move(prepared->bytes);
        responseCodec_ = std::make_unique<OriginResponseCodec>(
            std::move(prepared->responseExchange), hstsEnabled_, activeRoute_);
        bufferedRequest_.method = parsed.request().method();
        bufferedRequest_.target = routeTarget(parsed.request().target(), activeRoute_);
        bufferedRequest_.authority =
            website.origin_host_header().empty() || website.origin_host_header() == "$host"
                ? normalizeHostname(incomingHost)
                : website.origin_host_header();
        bufferedRequest_.headers.clear();
        for (const auto& header : parsed.request().headers()) {
            bufferedRequest_.headers.emplace_back(header.name(), header.value());
        }
        if (parsed.bodyPlan().chunked()) {
            auto decoded = decodeChunkedBody(parsed.wireBody(), kMaxRequestBytes);
            if (!decoded) {
                return false;
            }
            bufferedRequest_.body = std::move(*decoded);
            bufferedRequest_.hasBody = true;
        } else if (parsed.bodyPlan().knownLength()) {
            bufferedRequest_.body.assign(parsed.wireBody());
            bufferedRequest_.hasBody = true;
        }
        return true;
    }

    std::optional<PreparedOriginRequest> prepareBufferedOriginRequest() const {
        if (activeWebsite_ == nullptr) {
            return std::nullopt;
        }
        std::vector<ruvia::HttpHeaderView> headers;
        headers.reserve(bufferedRequest_.headers.size());
        for (const auto& [name, value] : bufferedRequest_.headers) {
            headers.emplace_back(name, value);
        }
        return flexedge::node::prepareOriginRequest({.method = bufferedRequest_.method,
                                                     .target = bufferedRequest_.target,
                                                     .headers = headers,
                                                     .body = bufferedRequest_.body,
                                                     .hasBody = bufferedRequest_.hasBody},
                                                    *activeWebsite_, requestHost_, clientAddress_,
                                                    secure_, false, false, false, activeRoute_);
    }

    void proxy(const ruvia::Http1ParsedRequest& parsed, const v2::Website& website,
               std::string_view incomingHost, const v2::RouteRule* route) {
        requestHost_.assign(incomingHost);
        activeWebsite_ = &website;
        activeRoute_ = route;
        const auto acceptsStreamingResponse = [&] {
            const auto accept = parsed.request().header("Accept");
            return accept && flexedge::node::acceptsEventStream(*accept);
        }();
        bufferedOriginEnabled_ = website.response_compression_enabled() &&
                                 parsed.request().header("Accept-Encoding").has_value() &&
                                 !upgradeRequested_ && !acceptsStreamingResponse;
        if (!prepareOriginRequest(parsed, website, incomingHost, acceptsStreamingResponse)) {
            send(response("502 Bad Gateway"));
            return;
        }
        connectTimeout_ = std::chrono::seconds(website.origin_connect_timeout_seconds());
        readTimeout_ = std::chrono::seconds(website.origin_read_timeout_seconds());
        tryNextOrigin();
    }

    void sendBufferedResponse(BufferedProxyResponse bufferedResponse) {
        const auto statusText = std::to_string(bufferedResponse.status);
        const auto status = ruvia::HttpStatusCode::fromValue(bufferedResponse.status);
        const auto reason = ruvia::httpReasonPhrase(status);
        std::size_t headerBytes = std::string_view("HTTP/1.1 ").size() + statusText.size() +
                                  std::string_view("\r\n").size();
        if (!reason.empty()) {
            headerBytes += 1 + reason.size();
        }
        for (const auto& [name, value] : bufferedResponse.headers) {
            if (shouldForwardBufferedResponseHeader(name, hstsEnabled_)) {
                headerBytes += name.size() + 2 + value.size() + 2;
            }
        }
        const auto bodySize = bufferedResponse.body.size();
        const auto sourceReservationBytes = bufferedResponse.reservation.bytes();
        const auto lengthText = std::to_string(bodySize);
        headerBytes += std::string_view("Content-Length: ").size() + lengthText.size() +
                       std::string_view("\r\nConnection: close\r\n").size();
        if (hstsEnabled_) {
            headerBytes +=
                std::string_view("Strict-Transport-Security: max-age=31536000\r\n").size();
        }
        headerBytes += 2;
        if (!bufferedResponse.reservation.tryGrow(headerBytes + bodySize)) {
            send(response("503 Service Unavailable"));
            return;
        }

        std::string wire;
        wire.reserve(headerBytes + bodySize);
        wire.append("HTTP/1.1 ").append(statusText);
        if (!reason.empty()) {
            wire.push_back(' ');
            wire.append(reason);
        }
        wire.append("\r\n");
        for (const auto& [name, value] : bufferedResponse.headers) {
            if (shouldForwardBufferedResponseHeader(name, hstsEnabled_)) {
                wire.append(name).append(": ").append(value).append("\r\n");
            }
        }
        wire.append("Content-Length: ").append(lengthText).append("\r\nConnection: close\r\n");
        if (hstsEnabled_) {
            wire.append("Strict-Transport-Security: max-age=31536000\r\n");
        }
        wire.append("\r\n").append(bufferedResponse.body);
        bufferedResponse.body.clear();
        bufferedResponse.body.shrink_to_fit();
        bufferedResponse.reservation.shrink(sourceReservationBytes);
        send(std::move(wire), std::move(bufferedResponse.reservation));
    }

    void completeBufferedOriginAttempt(std::uint64_t attempt, std::error_code error,
                                       BufferedProxyResponse response) {
        if (!originAttemptCurrent(attempt)) {
            return;
        }
        bufferedExchange_.reset();
        cancelTimer();
        if (error) {
            if (bufferedBytesExhausted(error)) {
                send(this->response("503 Service Unavailable"));
                return;
            }
            logIo("buffered origin exchange failed", error);
            health_.failure(websiteId_, currentOriginId_, unhealthyThreshold_);
            tryNextOrigin();
            return;
        }
        health_.success(websiteId_, currentOriginId_, healthyThreshold_);
        const auto* website = activeConfig_->website(bufferedRequest_.authority);
        if (website == nullptr) {
            website = activeConfig_->website(requestHost_);
        }
        if (website != nullptr) {
            applyResponseCompression(bufferedRequest_, *website, response);
        }
        if (activeRoute_ != nullptr) {
            applyRouteHeaders(response.headers, activeRoute_->response_headers());
        }
        sendBufferedResponse(std::move(response));
    }

    void startBufferedOriginAttempt(std::uint64_t attempt) {
        if (!originAttemptCurrent(attempt)) {
            return;
        }
        auto prepared = prepareBufferedOriginRequest();
        if (!prepared) {
            send(response("502 Bad Gateway"));
            return;
        }
        arm(connectTimeout_, attempt, TimeoutAction::kFailover);
        const auto self = this->shared_from_this();
        bufferedExchange_ = std::make_shared<BufferedOriginExchange>(
            stream_.get_executor(), originConnections_, std::move(prepared->bytes),
            std::move(prepared->responseExchange), originHost_, originPort_, originSecure_,
            connectTimeout_ + readTimeout_, responseBuffers_,
            [self, attempt](std::error_code error, BufferedProxyResponse response) {
                self->completeBufferedOriginAttempt(attempt, std::move(error), std::move(response));
            });
        bufferedExchange_->start();
    }

    void completeOriginConnection(std::uint64_t attempt, const std::error_code& error) {
        if (!originAttemptCurrent(attempt)) {
            return;
        }
        if (error) {
            logIo("origin connect failed", error);
            health_.failure(websiteId_, currentOriginId_, unhealthyThreshold_);
            tryNextOrigin();
            return;
        }
        writeOriginRequest(attempt);
    }

    void startDirectOriginAttempt(std::uint64_t attempt) {
        originConnectionKey_ = {
            .host = originHost_,
            .port = originPort_,
            .secure = originSecure_,
        };
        originTransport_ = originConnections_.acquire(*originConnectionKey_);
        reusedOriginTransport_ = originTransport_->connected();
        if (reusedOriginTransport_) {
            writeOriginRequest(attempt);
            return;
        }
        arm(connectTimeout_, attempt, TimeoutAction::kFailover);
        const auto self = this->shared_from_this();
        originTransport_->connect(originHost_, originPort_, originSecure_, false,
                                  [self, attempt](const std::error_code& error, HttpWireProtocol) {
                                      self->completeOriginConnection(attempt, error);
                                  });
    }

    void resetOriginAttempt() {
        ++originAttempt_;
        cancelTimer();
        if (bufferedExchange_) {
            bufferedExchange_->close();
            bufferedExchange_.reset();
        }
        discardOriginTransport();
    }

    void tryNextOrigin() {
        resetOriginAttempt();
        if (originIndex_ >= origins_.size()) {
            send(response("502 Bad Gateway"));
            return;
        }
        const auto* origin = origins_[originIndex_++];
        originHost_ = origin->host();
        currentOriginId_ = origin->id();
        originPort_ = static_cast<std::uint16_t>(origin->port());
        originSecure_ = origin->protocol() == "https";
        const auto attempt = originAttempt_;
        if (bufferedOriginEnabled_) {
            startBufferedOriginAttempt(attempt);
            return;
        }
        startDirectOriginAttempt(attempt);
    }

    void writeOriginRequest(std::uint64_t attempt) {
        if (!originAttemptCurrent(attempt)) {
            return;
        }
        arm(connectTimeout_, attempt, TimeoutAction::kFailover);
        const auto self = this->shared_from_this();
        auto completion = [self, attempt](const std::error_code& error, std::size_t size) {
            if (!self->originAttemptCurrent(attempt)) {
                return;
            }
            if (error) {
                if (size == 0 && self->retryStaleOriginTransport()) {
                    return;
                }
                self->logIo("origin request write failed", error);
                self->health_.failure(self->websiteId_, self->currentOriginId_,
                                      self->unhealthyThreshold_);
                self->tryNextOrigin();
                return;
            }
            self->readOriginHead(attempt);
        };
        originTransport_->write(asio::buffer(outboundRequest_), std::move(completion));
    }

    void readOriginHead(std::uint64_t attempt) {
        if (!originAttemptCurrent(attempt)) {
            return;
        }
        arm(readTimeout_, attempt, TimeoutAction::kFailover);
        const auto self = this->shared_from_this();
        auto completion = [self, attempt](const std::error_code& error, std::size_t size) {
            if (!self->originAttemptCurrent(attempt)) {
                return;
            }
            if (error) {
                self->logIo("origin response head read failed", error);
                self->health_.failure(self->websiteId_, self->currentOriginId_,
                                      self->unhealthyThreshold_);
                self->tryNextOrigin();
                return;
            }
            const auto status = self->responseCodec_->consumeHead(
                std::string_view(self->originReadBuffer_.data(), size));
            if (status == OriginResponseHeadStatus::kNeedMore) {
                self->readOriginHead(attempt);
                return;
            }
            if (status == OriginResponseHeadStatus::kFailure) {
                self->log("origin response head parse failed");
                self->health_.failure(self->websiteId_, self->currentOriginId_,
                                      self->unhealthyThreshold_);
                self->tryNextOrigin();
                return;
            }
            self->tunnel_ = self->responseCodec_->tunnel();
            self->relayBytes_ = self->responseCodec_->takeOutput();
            self->captureResponseHeaders(self->relayBytes_);
            self->health_.success(self->websiteId_, self->currentOriginId_,
                                  self->healthyThreshold_);
            self->relayOriginBytes(attempt);
        };
        originTransport_->read(asio::buffer(originReadBuffer_), std::move(completion));
    }

    void relayOriginBytes(std::uint64_t attempt) {
        if (!originAttemptCurrent(attempt)) {
            return;
        }
        arm(readTimeout_, attempt);
        const auto self = this->shared_from_this();
        asio::async_write(
            stream_, asio::buffer(relayBytes_),
            [self, attempt](const std::error_code& error, std::size_t size) {
                if (!self->originAttemptCurrent(attempt)) {
                    return;
                }
                self->metrics_.trafficOut(size);
                self->proxiedResponseBytes_ += size;
                if (error) {
                    if (self->accessLogClientAbortEnabled_) {
                        self->recordAccess(499, self->proxiedResponseBytes_);
                    }
                    self->logIo("client relay write failed", error);
                    self->close();
                    return;
                }
                if (self->tunnel_) {
                    self->recordAccess(self->responseCodec_->statusCode().value_or(101),
                                       self->proxiedResponseBytes_);
                    self->startTunnel(attempt);
                } else if (self->responseCodec_->complete()) {
                    self->recordAccess(self->responseCodec_->statusCode().value_or(502),
                                       self->proxiedResponseBytes_);
                    self->releaseOriginTransport();
                    self->close();
                } else {
                    self->readOriginBody(attempt);
                }
            });
    }

    void startTunnel(std::uint64_t attempt) {
        if (!originAttemptCurrent(attempt)) {
            return;
        }
        cancelTimer();
        if (clientUpgradeRemainder_.empty()) {
            readTunnelClient(attempt);
            readTunnelOrigin(attempt);
            return;
        }
        const auto self = this->shared_from_this();
        auto completion = [self, attempt](const std::error_code& error, std::size_t) {
            if (!self->originAttemptCurrent(attempt)) {
                return;
            }
            if (error) {
                self->close();
                return;
            }
            self->clientUpgradeRemainder_.clear();
            self->readTunnelClient(attempt);
            self->readTunnelOrigin(attempt);
        };
        originTransport_->write(asio::buffer(clientUpgradeRemainder_), std::move(completion));
    }

    void readTunnelClient(std::uint64_t attempt) {
        if (!originAttemptCurrent(attempt)) {
            return;
        }
        const auto self = this->shared_from_this();
        stream_.async_read_some(asio::buffer(clientTunnelBuffer_),
                                [self, attempt](const std::error_code& error, std::size_t size) {
                                    if (!self->originAttemptCurrent(attempt)) {
                                        return;
                                    }
                                    if (error) {
                                        self->close();
                                        return;
                                    }
                                    self->writeTunnelOrigin(attempt, size);
                                });
    }

    void writeTunnelOrigin(std::uint64_t attempt, std::size_t size) {
        if (!originAttemptCurrent(attempt)) {
            return;
        }
        const auto self = this->shared_from_this();
        auto completion = [self, attempt](const std::error_code& error, std::size_t) {
            if (!self->originAttemptCurrent(attempt)) {
                return;
            }
            if (error) {
                self->close();
                return;
            }
            self->readTunnelClient(attempt);
        };
        originTransport_->write(asio::buffer(clientTunnelBuffer_.data(), size),
                                std::move(completion));
    }

    void readTunnelOrigin(std::uint64_t attempt) {
        if (!originAttemptCurrent(attempt)) {
            return;
        }
        const auto self = this->shared_from_this();
        auto completion = [self, attempt](const std::error_code& error, std::size_t size) {
            if (!self->originAttemptCurrent(attempt)) {
                return;
            }
            if (error) {
                self->close();
                return;
            }
            self->writeTunnelClient(attempt, size);
        };
        originTransport_->read(asio::buffer(originTunnelBuffer_), std::move(completion));
    }

    void writeTunnelClient(std::uint64_t attempt, std::size_t size) {
        if (!originAttemptCurrent(attempt)) {
            return;
        }
        const auto self = this->shared_from_this();
        asio::async_write(stream_, asio::buffer(originTunnelBuffer_.data(), size),
                          [self, attempt](const std::error_code& error, std::size_t size) {
                              if (!self->originAttemptCurrent(attempt)) {
                                  return;
                              }
                              self->metrics_.trafficOut(size);
                              if (error) {
                                  self->close();
                                  return;
                              }
                              self->readTunnelOrigin(attempt);
                          });
    }

    void readOriginBody(std::uint64_t attempt) {
        if (!originAttemptCurrent(attempt)) {
            return;
        }
        arm(readTimeout_, attempt);
        const auto self = this->shared_from_this();
        auto completion = [self, attempt](const std::error_code& error, std::size_t size) {
            if (!self->originAttemptCurrent(attempt)) {
                return;
            }
            if (error == asio::error::eof || error == asio::ssl::error::stream_truncated) {
                if (self->responseCodec_) {
                    self->recordAccess(self->responseCodec_->statusCode().value_or(502),
                                       self->proxiedResponseBytes_);
                }
                self->close();
                return;
            }
            if (error) {
                self->logIo("origin response body read failed", error);
                self->close();
                return;
            }
            if (!self->responseCodec_->consumeBody(
                    std::string_view(self->originReadBuffer_.data(), size))) {
                self->log("origin response body parse failed");
                self->close();
                return;
            }
            self->relayBytes_.assign(self->originReadBuffer_.data(), size);
            self->relayOriginBytes(attempt);
        };
        originTransport_->read(asio::buffer(originReadBuffer_), std::move(completion));
    }

    void consume() noexcept {
        try {
            consumeChecked();
        } catch (const std::exception& error) {
            log("request handling failed", error);
            close();
        } catch (...) {
            log("request handling failed with unknown exception");
            close();
        }
    }

    void consumeChecked() {
        const auto result = parser_.parse(requestBytes_);
        if (const auto* parsed = result.parsed()) {
            handleRequest(*parsed);
            return;
        }
        if (result.failure()) {
            send(response("400 Bad Request"));
            return;
        }
        const auto* more = result.needMore();
        if (more == nullptr || requestBytes_.size() >= kMaxRequestBytes) {
            send(response("413 Content Too Large"));
            return;
        }
        const auto requiredTotalBytes = more->requiredTotalBytes();
        if (requiredTotalBytes && *requiredTotalBytes > kMaxRequestBytes) {
            send(response("413 Content Too Large"));
            return;
        }
        read();
    }

    void read() {
        if (closed_ || responsePending_) {
            return;
        }
        const auto self = this->shared_from_this();
        stream_.async_read_some(asio::buffer(readBuffer_),
                                [self](const std::error_code& error, std::size_t size) {
                                    self->readCompleted(error, size);
                                });
    }

    void readCompleted(const std::error_code& error, std::size_t size) noexcept {
        if (closed_ || responsePending_) {
            return;
        }
        if (error) {
            close();
            return;
        }
        try {
            if (!requestReservation_.tryGrow(size)) {
                send(response("503 Service Unavailable"));
                return;
            }
            requestBytes_.append(readBuffer_.data(), size);
            consume();
        } catch (const std::exception& error) {
            log("client input handling failed", error);
            close();
        } catch (...) {
            log("client input handling failed with unknown exception");
            close();
        }
    }

    [[nodiscard]] decltype(auto) clientSocket() noexcept { return stream_.lowest_layer(); }

    ClientStream stream_;
    OriginConnectionPool& originConnections_;
    asio::steady_timer timer_;
    RuntimeState& runtime_;
    OriginHealthRegistry& health_;
    RuntimeMetrics& metrics_;
    NodeLogBuffer::Producer& logs_;
    BufferedBytesBudget& responseBuffers_;
    ruvia::Http1RequestParser parser_;
    std::array<char, 8192> readBuffer_{};
    std::string requestBytes_;
    std::string responseBytes_;
    BufferedBytesLease requestReservation_;
    BufferedBytesLease responseReservation_;
    std::string originHost_;
    std::string websiteId_;
    std::string requestHost_;
    std::string currentOriginId_;
    std::string clientAddress_;
    std::string requestMethod_;
    std::string requestTarget_;
    std::string requestUserAgent_;
    std::string requestReferer_;
    std::string requestHeaders_;
    std::string requestBody_;
    std::string responseHeaders_;
    std::string queryString_;
    std::string cookies_;
    std::string tlsFingerprint_;
    std::uint16_t originPort_{};
    std::chrono::seconds connectTimeout_{10};
    std::chrono::seconds readTimeout_{30};
    std::string outboundRequest_;
    std::unique_ptr<OriginTransport> originTransport_;
    std::optional<OriginConnectionKey> originConnectionKey_;
    std::unique_ptr<OriginResponseCodec> responseCodec_;
    std::shared_ptr<BufferedOriginExchange> bufferedExchange_;
    BufferedProxyRequest bufferedRequest_;
    std::array<char, 8192> originReadBuffer_{};
    std::array<char, 16384> clientTunnelBuffer_{};
    std::array<char, 16384> originTunnelBuffer_{};
    std::string relayBytes_;
    std::string clientUpgradeRemainder_;
    std::shared_ptr<const CompiledConfig> activeConfig_;
    const v2::Website* activeWebsite_{};
    const v2::RouteRule* activeRoute_{};
    std::vector<const v2::Origin*> origins_;
    std::size_t originIndex_{};
    std::uint64_t originAttempt_{};
    std::uint64_t proxiedResponseBytes_{};
    std::uint32_t healthyThreshold_{1};
    std::uint32_t unhealthyThreshold_{1};
    std::uint64_t sequence_{};
    std::shared_ptr<const void> transportState_;
    bool secure_{};
    bool originSecure_{};
    bool bufferedOriginEnabled_{};
    bool reusedOriginTransport_{};
    bool retriedReusedOriginTransport_{};
    bool upgradeRequested_{};
    bool tunnel_{};
    bool hstsEnabled_{};
    bool closed_{};
    bool responsePending_{};
    bool accessRecorded_{};
    bool accessLogEnabled_{};
    bool accessLogResponseHeadersEnabled_{};
    bool accessLogClientAbortEnabled_{};
    bool requestBodyTruncated_{};
    std::array<bool, 5> accessLogStatusCodeRanges_{true, true, true, true, true};
    std::chrono::steady_clock::time_point requestStartedAt_{std::chrono::steady_clock::now()};
};

class HttpListener final : public std::enable_shared_from_this<HttpListener> {
  public:
    HttpListener(ruvia::EventLoop owner, RuntimeState& runtime, OriginHealthRegistry& health,
                 RuntimeMetrics& metrics, NodeLogBuffer::Producer& logs,
                 OriginConnectionPool& originConnections, BufferedBytesBudget& requestBuffers,
                 BufferedBytesBudget& responseBuffers, const asio::ip::tcp::endpoint& endpoint)
        : owner_(std::move(owner)), runtime_(runtime), health_(health), metrics_(metrics),
          logs_(logs), originConnections_(originConnections), requestBuffers_(requestBuffers),
          responseBuffers_(responseBuffers), acceptor_(owner_.ioContext()),
          activationTimer_(owner_.ioContext()) {
        std::error_code error;
        error = acceptor_.open(endpoint.protocol(), error);
        if (!error) {
            error = acceptor_.set_option(asio::socket_base::reuse_address(true), error);
        }
#if defined(SO_REUSEPORT) && !defined(_WIN32)
        if (!error) {
            int enabled = 1;
            if (::setsockopt(acceptor_.native_handle(), SOL_SOCKET, SO_REUSEPORT, &enabled,
                             sizeof(enabled)) != 0) {
                throw std::system_error(errno, std::generic_category(),
                                        "could not enable SO_REUSEPORT");
            }
        }
#endif
        if (!error) {
            error = acceptor_.bind(endpoint, error);
        }
        if (!error) {
            error = acceptor_.listen(asio::socket_base::max_listen_connections, error);
        }
        if (error) {
            throw std::system_error(error, "could not start edge HTTP listener");
        }
        stopRegistration_ = owner_.onStop([this] { stop(); });
    }

    ~HttpListener() { stop(); }

    void start() {
        if (activationGate_ && !activationGate_->active()) {
            activationTimer_.expires_after(std::chrono::milliseconds(1));
            const auto self = shared_from_this();
            activationTimer_.async_wait([self](const std::error_code& error) {
                if (!error) {
                    self->start();
                }
            });
            return;
        }
        accept();
    }

    void requestStart(std::shared_ptr<const ListenerActivationGate> activationGate = nullptr) {
        activationGate_ = std::move(activationGate);
        if (owner_.isCurrent()) {
            start();
            return;
        }
        const auto self = shared_from_this();
        if (!owner_.post([self] { self->start(); }).accepted()) {
            throw std::runtime_error("edge HTTP listener worker is stopping");
        }
    }

    [[nodiscard]] asio::ip::tcp::endpoint localEndpoint() const {
        return acceptor_.local_endpoint();
    }

    void stop() noexcept {
        std::error_code ignored;
        activationTimer_.cancel(ignored);
        ignored = acceptor_.cancel(ignored);
        ignored = acceptor_.close(ignored);
    }

    void requestStop() noexcept {
        if (owner_.isCurrent()) {
            stop();
            return;
        }
        const auto self = shared_from_this();
        if (!owner_.post([self] { self->stop(); }).accepted()) {
            stop();
        }
    }

  private:
    void accept() {
        if (!acceptor_.is_open()) {
            return;
        }
        auto socket = std::make_shared<asio::ip::tcp::socket>(owner_.ioContext());
        const auto self = shared_from_this();
        acceptor_.async_accept(
            *socket,
            asio::bind_executor(owner_.executor(), [self, socket](const std::error_code& error) {
                if (!error) {
                    std::make_shared<BasicHttpSession<asio::ip::tcp::socket>>(
                        std::move(*socket), self->runtime_, self->health_, self->metrics_,
                        self->logs_, self->originConnections_, self->requestBuffers_,
                        self->responseBuffers_, ++self->sequence_)
                        ->start();
                }
                if (self->acceptor_.is_open()) {
                    self->accept();
                }
            }));
    }

    ruvia::EventLoop owner_;
    RuntimeState& runtime_;
    OriginHealthRegistry& health_;
    RuntimeMetrics& metrics_;
    NodeLogBuffer::Producer& logs_;
    OriginConnectionPool& originConnections_;
    BufferedBytesBudget& requestBuffers_;
    BufferedBytesBudget& responseBuffers_;
    asio::ip::tcp::acceptor acceptor_;
    asio::steady_timer activationTimer_;
    ruvia::EventLoopStopRegistration stopRegistration_;
    std::shared_ptr<const ListenerActivationGate> activationGate_;
    std::uint64_t sequence_{};
};

} // namespace flexedge::node
