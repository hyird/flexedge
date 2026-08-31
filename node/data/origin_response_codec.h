#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include <ruvia/http/Http1ClientResponseParser.h>

#include "node/data/chunked_wire_tracker.h"
#include "node/data/origin_request_codec.h"

namespace flexedge::node {

enum class OriginResponseHeadStatus { kNeedMore, kReady, kFailure };

class OriginResponseCodec final {
  public:
    explicit OriginResponseCodec(ruvia::Http1ClientExchangeState responseExchange, bool hstsEnabled,
                                 const v2::RouteRule* route = nullptr)
        : parser_(std::make_unique<ruvia::Http1ClientResponseParser>(std::move(responseExchange))),
          hstsEnabled_(hstsEnabled), route_(route) {}

    [[nodiscard]] OriginResponseHeadStatus consumeHead(std::string_view bytes) {
        headBytes_.append(bytes);
        for (;;) {
            auto result = parser_->parse(std::string_view(headBytes_).substr(headOffset_));
            if (result.needMore()) {
                return headBytes_.size() >= kMaxHeadBytes ? OriginResponseHeadStatus::kFailure
                                                          : OriginResponseHeadStatus::kNeedMore;
            }
            const auto* parsed = result.parsed();
            if (parsed == nullptr) {
                return OriginResponseHeadStatus::kFailure;
            }
            const auto headStart = headOffset_;
            headOffset_ += parsed->consumedBytes();
            if (parsed->plan().informational()) {
                if (headOffset_ == headBytes_.size()) {
                    return OriginResponseHeadStatus::kNeedMore;
                }
                continue;
            }
            tunnel_ = parsed->plan().protocolUpgrade() != nullptr;
            const auto body = std::string_view(headBytes_).substr(headOffset_);
            if (!configureBody(parsed->plan(), body)) {
                return OriginResponseHeadStatus::kFailure;
            }
            statusCode_ = static_cast<std::uint16_t>(parsed->head().status().value());
            output_.assign(headBytes_, 0, headStart);
            appendHead(parsed->head());
            if (tunnel_ || bodyKind_ != BodyKind::kNone) {
                output_.append(body);
            }
            return OriginResponseHeadStatus::kReady;
        }
    }

    [[nodiscard]] bool consumeBody(std::string_view bytes) {
        if (complete_) {
            return bytes.empty();
        }
        if (bodyKind_ == BodyKind::kKnownLength) {
            if (bytes.size() > remaining_) {
                return false;
            }
            remaining_ -= bytes.size();
            complete_ = remaining_ == 0;
            return true;
        }
        if (bodyKind_ == BodyKind::kChunked) {
            const auto status = chunked_.consume(bytes);
            complete_ = status == ChunkedWireStatus::kComplete;
            return status != ChunkedWireStatus::kInvalid;
        }
        return bodyKind_ == BodyKind::kCloseDelimited;
    }

    [[nodiscard]] std::string takeOutput() { return std::move(output_); }
    [[nodiscard]] bool complete() const noexcept { return complete_; }
    [[nodiscard]] bool tunnel() const noexcept { return tunnel_; }
    [[nodiscard]] bool originReusable() const noexcept {
        return complete_ && !tunnel_ && originReusable_;
    }
    [[nodiscard]] bool eofCompletesResponse() const noexcept {
        return bodyKind_ == BodyKind::kCloseDelimited;
    }
    [[nodiscard]] std::optional<std::uint16_t> statusCode() const noexcept { return statusCode_; }

  private:
    static constexpr std::size_t kMaxHeadBytes = 1024 * 1024;
    enum class BodyKind { kNone, kKnownLength, kChunked, kCloseDelimited, kTunnel };

    static bool responseHopByHop(std::string_view name) noexcept {
        return httpHeaderName(name, "Connection") || httpHeaderName(name, "Keep-Alive") ||
               httpHeaderName(name, "Proxy-Connection") || httpHeaderName(name, "Upgrade");
    }

    static std::vector<std::string> connectionTokens(const ruvia::HttpClientResponseHead& head) {
        std::vector<std::string> connectionTokens;
        for (const auto& header : head.headers()) {
            if (!httpHeaderName(header.name(), "Connection")) {
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
                    connectionTokens.emplace_back(token);
                }
                if (comma == std::string_view::npos) {
                    break;
                }
                value.remove_prefix(comma + 1);
            }
        }
        return connectionTokens;
    }

    void appendStatusLine(const ruvia::HttpClientResponseHead& head) {
        output_.append("HTTP/1.1 ");
        output_.append(std::to_string(head.status().value()));
        const auto reason = ruvia::httpReasonPhrase(head.status());
        if (!reason.empty()) {
            output_.push_back(' ');
            output_.append(reason);
        }
        output_.append("\r\n");
    }

    void appendResponseHeaders(const ruvia::HttpClientResponseHead& head,
                               const std::vector<std::string>& connectionTokens) {
        for (const auto& header : head.headers()) {
            const auto preserveUpgrade = tunnel_ && (httpHeaderName(header.name(), "Connection") ||
                                                     httpHeaderName(header.name(), "Upgrade"));
            const auto namedByConnection =
                std::ranges::any_of(connectionTokens, [&](const auto& token) {
                    return httpHeaderName(header.name(), token);
                });
            if ((!preserveUpgrade && (responseHopByHop(header.name()) || namedByConnection)) ||
                (hstsEnabled_ && httpHeaderName(header.name(), "Strict-Transport-Security")) ||
                (route_ != nullptr &&
                 std::ranges::any_of(route_->response_headers(), [&](const auto& mutation) {
                     return httpHeaderName(header.name(), mutation.name());
                 }))) {
                continue;
            }
            output_.append(header.name());
            output_.append(": ");
            output_.append(header.value());
            output_.append("\r\n");
        }
        if (route_ != nullptr) {
            for (const auto& mutation : route_->response_headers()) {
                output_.append(mutation.name());
                output_.append(": ");
                output_.append(mutation.value());
                output_.append("\r\n");
            }
        }
    }

    void appendProxyHeaders() {
        if (!tunnel_) {
            output_.append("Connection: close\r\n");
        }
        if (hstsEnabled_ && !tunnel_) {
            output_.append("Strict-Transport-Security: max-age=31536000\r\n");
        }
        output_.append("\r\n");
    }

    void appendHead(const ruvia::HttpClientResponseHead& head) {
        const auto connectionTokenValues = connectionTokens(head);
        appendStatusLine(head);
        appendResponseHeaders(head, connectionTokenValues);
        appendProxyHeaders();
    }

    bool configureBody(const ruvia::Http1ClientResponsePlan& plan, std::string_view initialBody) {
        originReusable_ = planAllowsReuse(plan);
        if (tunnel_) {
            bodyKind_ = BodyKind::kTunnel;
            return true;
        }
        if (const auto* known = plan.knownLength()) {
            bodyKind_ = BodyKind::kKnownLength;
            remaining_ = known->contentLength();
            return consumeBody(initialBody);
        }
        if (plan.chunked()) {
            bodyKind_ = BodyKind::kChunked;
            return consumeBody(initialBody);
        }
        if (plan.closeDelimited()) {
            bodyKind_ = BodyKind::kCloseDelimited;
            return true;
        }
        if (const auto* zero = plan.zeroContent()) {
            if (const auto* known = zero->knownLength()) {
                bodyKind_ = BodyKind::kKnownLength;
                remaining_ = known->contentLength();
                return consumeBody(initialBody);
            }
            if (zero->chunked()) {
                bodyKind_ = BodyKind::kChunked;
                return consumeBody(initialBody);
            }
            bodyKind_ = BodyKind::kCloseDelimited;
            return true;
        }
        bodyKind_ = BodyKind::kNone;
        complete_ = true;
        return initialBody.empty();
    }

    static bool planAllowsReuse(const ruvia::Http1ClientResponsePlan& plan) noexcept {
        const auto allowsReuse = [](const auto* response) {
            return response->persistence() == ruvia::Http1ClosePolicy::kAllowReuse;
        };
        if (const auto* response = plan.withoutContent()) {
            return allowsReuse(response);
        }
        if (const auto* response = plan.knownLength()) {
            return allowsReuse(response);
        }
        if (const auto* response = plan.chunked()) {
            return allowsReuse(response);
        }
        if (const auto* response = plan.zeroContent()) {
            if (const auto* known = response->knownLength()) {
                return allowsReuse(known);
            }
            if (const auto* chunked = response->chunked()) {
                return allowsReuse(chunked);
            }
        }
        return false;
    }

    std::unique_ptr<ruvia::Http1ClientResponseParser> parser_;
    std::string headBytes_;
    std::size_t headOffset_{};
    std::string output_;
    BodyKind bodyKind_{BodyKind::kNone};
    std::size_t remaining_{};
    ChunkedWireTracker chunked_;
    std::optional<std::uint16_t> statusCode_;
    bool hstsEnabled_{};
    const v2::RouteRule* route_{};
    bool tunnel_{};
    bool complete_{};
    bool originReusable_{};
};

} // namespace flexedge::node
