#pragma once

#include <ruvia/core/channel.h>
#include <ruvia/core/event_loop.h>
#include <ruvia/core/task.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <openssl/rand.h>

#include "node/proto/edge_control.pb.h"
#include "node/proto/log_contract.h"

namespace flexedge::node {

// A published batch owns all event storage and exposes no mutable aliases or
// producer runtime. The control worker adds the authenticated node identity.
class log_batch final {
public:
    explicit log_batch(v2::LogDelivery value)
        : value_(std::move(value)) {
        if (!value_.node_id().empty() || value_.events_size() == 0 ||
            static_cast<std::size_t>(value_.events_size()) > log_contract::max_delivery_events ||
            value_.ByteSizeLong() > log_contract::max_delivery_bytes - node_identity_wire_bytes) {
            throw std::invalid_argument("invalid node log batch");
        }
    }

    [[nodiscard]] const v2::LogDelivery& value() const noexcept {
        return value_;
    }

    static constexpr std::size_t node_identity_wire_bytes{128};

    static std::size_t event_wire_bytes(std::size_t bytes) noexcept {
        auto value = bytes;
        std::size_t size{2};
        while (value >= 0x80) {
            ++size;
            value >>= 7;
        }
        return size + bytes;
    }

private:
    v2::LogDelivery value_;
};

using owned_log_batch = std::unique_ptr<const log_batch>;

struct log_production_snapshot final {
    std::uint64_t published_events_{};
    std::uint64_t queued_events_{};
    std::uint64_t dropped_events_{};
};

inline constexpr std::size_t data_log_channel_capacity{62};
inline constexpr std::size_t control_log_channel_capacity{6};

// Each producer belongs to exactly one worker. Its mutable staging batch and
// counters are never inspected by the control worker; only sealed batches cross
// the bounded channel. Full/closed channels drop the new batch locally.
class log_producer final {
public:
    struct access_record final {
        std::string website_id_{};
        std::string client_ip_{};
        std::string protocol_{};
        std::string method_{};
        std::string host_{};
        std::string target_{};
        std::uint32_t status_code_{};
        std::uint64_t request_bytes_{};
        std::uint64_t response_bytes_{};
        std::uint64_t duration_ms_{};
        std::string user_agent_{};
        std::string referer_{};
        std::string request_headers_{};
        std::string request_body_{};
        bool request_body_truncated_{};
        std::string tls_fingerprint_{};
        std::string response_headers_{};
        std::string query_string_{};
        std::string cookies_{};
    };

    static constexpr std::size_t max_request_headers_bytes{log_contract::max_request_headers_bytes};
    static constexpr std::size_t max_response_headers_bytes{log_contract::max_response_headers_bytes};
    static constexpr std::size_t max_request_body_bytes{log_contract::max_request_body_bytes};
    static constexpr std::size_t max_query_string_bytes{log_contract::max_query_string_bytes};
    static constexpr std::size_t max_cookies_bytes{log_contract::max_cookies_bytes};

    log_producer(ruvia::event_loop owner, ruvia::channel_sender<owned_log_batch> sender)
        : state_(std::make_shared<producer_state>(std::move(owner), std::move(sender))),
          stop_registration_(state_->owner_.on_stop([state = state_]() -> ruvia::task<void> {
              (void)state->flush();
              co_return;
          })) {}

    log_producer(const log_producer&) = delete;
    log_producer& operator=(const log_producer&) = delete;

    bool access(access_record record) {
        return state_->append(access_event(std::move(record)));
    }

    bool node(std::string_view level, std::string_view category, std::string_view message) {
        return state_->append(node_event(level, category, message));
    }

    bool flush() {
        return state_->flush();
    }

    [[nodiscard]] std::uint64_t dropped_events() const {
        state_->require_owner();
        return state_->dropped_events_;
    }

    [[nodiscard]] std::size_t queued_events() const {
        state_->require_owner();
        return static_cast<std::size_t>(state_->pending_.events_size());
    }

    [[nodiscard]] log_production_snapshot snapshot() const {
        state_->require_owner();
        return {.published_events_ = state_->published_events_,
            .queued_events_ = static_cast<std::uint64_t>(state_->pending_.events_size()),
            .dropped_events_ = state_->dropped_events_};
    }

private:
    // Shared ownership only retains callbacks on this same worker. This state
    // is never passed through the cross-worker channel or exposed to receivers.
    struct producer_state final : std::enable_shared_from_this<producer_state> {
        producer_state(ruvia::event_loop owner, ruvia::channel_sender<owned_log_batch> sender)
            : owner_(std::move(owner)),
              sender_(std::move(sender)) {}

        void require_owner() const {
            if (!owner_.is_current()) {
                throw std::logic_error("log production must run on its owning worker");
            }
        }

        bool append(v2::LogEvent event) {
            require_owner();
            const auto bytes = log_batch::event_wire_bytes(event.ByteSizeLong());
            if (bytes > log_contract::max_delivery_bytes - log_batch::node_identity_wire_bytes) {
                ++dropped_events_;
                return false;
            }
            if (wire_bytes_ + bytes > log_contract::max_delivery_bytes) {
                (void)flush();
            }
            *pending_.add_events() = std::move(event);
            wire_bytes_ += bytes;
            if (static_cast<std::size_t>(pending_.events_size()) == log_contract::max_delivery_events) {
                return flush();
            }
            if (!flush_scheduled_) {
                flush_scheduled_ = true;
                const auto posted = owner_.post([self = shared_from_this()] {
                    self->flush_scheduled_ = false;
                    (void)self->flush();
                });
                if (!posted.accepted()) {
                    flush_scheduled_ = false;
                    dropped_events_ += static_cast<std::uint64_t>(pending_.events_size());
                    pending_.Clear();
                    wire_bytes_ = log_batch::node_identity_wire_bytes;
                    return false;
                }
            }
            return true;
        }

        bool flush() {
            require_owner();
            if (pending_.events_size() == 0) {
                return true;
            }
            const auto count = static_cast<std::uint64_t>(pending_.events_size());
            auto batch = std::make_unique<const log_batch>(std::move(pending_));
            pending_ = v2::LogDelivery{};
            wire_bytes_ = log_batch::node_identity_wire_bytes;
            const auto sent = sender_.send(std::move(batch));
            if (!sent.accepted()) {
                dropped_events_ += count;
                return false;
            }
            published_events_ += count;
            return true;
        }

        ruvia::event_loop owner_;
        ruvia::channel_sender<owned_log_batch> sender_;
        v2::LogDelivery pending_;
        std::size_t wire_bytes_{log_batch::node_identity_wire_bytes};
        std::uint64_t dropped_events_{};
        std::uint64_t published_events_{};
        bool flush_scheduled_{};
    };

    static std::uint64_t now_ms() {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
                .count());
    }

    static std::string uuid() {
        unsigned char bytes[16]{};
        if (RAND_bytes(bytes, sizeof(bytes)) != 1) {
            const auto value = now_ms();
            for (std::size_t index = 0; index < sizeof(bytes); ++index) {
                bytes[index] = static_cast<unsigned char>((value >> ((index % 8) * 8)) & 0xff);
            }
        }
        bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0f) | 0x40);
        bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3f) | 0x80);
        constexpr char hex[] = "0123456789abcdef";
        std::string result;
        result.reserve(36);
        for (std::size_t index = 0; index < sizeof(bytes); ++index) {
            if (index == 4 || index == 6 || index == 8 || index == 10) {
                result.push_back('-');
            }
            result.push_back(hex[bytes[index] >> 4]);
            result.push_back(hex[bytes[index] & 0x0f]);
        }
        return result;
    }

    static std::string bounded(std::string_view value, std::size_t maximum) {
        value = value.substr(0, maximum);
        return std::string(value);
    }

    static std::string printable(std::string_view value, std::size_t maximum) {
        value = value.substr(0, maximum);
        std::string result;
        result.reserve(value.size());
        for (const auto ch : value) {
            const auto byte = static_cast<unsigned char>(ch);
            if (byte == '\n' || byte == '\r' || byte == '\t' || (byte >= 0x20 && byte <= 0x7e)) {
                result.push_back(static_cast<char>(byte));
            } else {
                result.push_back('.');
            }
        }
        return result;
    }

    static access_record clamp(access_record record) {
        record.website_id_ = bounded(record.website_id_, log_contract::max_website_id_bytes);
        record.client_ip_ = bounded(record.client_ip_, log_contract::max_client_ip_bytes);
        record.protocol_ = bounded(record.protocol_, log_contract::max_protocol_bytes);
        record.method_ = bounded(record.method_, log_contract::max_method_bytes);
        record.host_ = bounded(record.host_, log_contract::max_host_bytes);
        record.target_ = bounded(record.target_, log_contract::max_target_bytes);
        record.status_code_ = std::clamp(record.status_code_, 100u, 999u);
        record.request_bytes_ = (std::min)(record.request_bytes_, log_contract::max_request_bytes);
        record.response_bytes_ = (std::min)(record.response_bytes_, log_contract::max_response_bytes);
        record.duration_ms_ = (std::min)(record.duration_ms_, log_contract::max_duration_ms);
        record.user_agent_ = bounded(record.user_agent_, log_contract::max_user_agent_bytes);
        record.referer_ = bounded(record.referer_, log_contract::max_referer_bytes);
        record.request_body_truncated_ =
            record.request_body_truncated_ || record.request_body_.size() > max_request_body_bytes;
        record.request_headers_ = printable(record.request_headers_, max_request_headers_bytes);
        record.response_headers_ = printable(record.response_headers_, max_response_headers_bytes);
        record.request_body_ = printable(record.request_body_, max_request_body_bytes);
        record.tls_fingerprint_ =
            bounded(record.tls_fingerprint_, log_contract::max_tls_fingerprint_bytes);
        record.query_string_ = printable(record.query_string_, max_query_string_bytes);
        record.cookies_ = printable(record.cookies_, max_cookies_bytes);
        return record;
    }

    static v2::LogEvent access_event(access_record record) {
        record = clamp(std::move(record));
        v2::LogEvent event;
        auto* value = event.mutable_access_log();
        value->set_id(uuid());
        value->set_occurred_unix_ms(now_ms());
        value->set_website_id(record.website_id_);
        value->set_client_ip(record.client_ip_);
        value->set_protocol(record.protocol_);
        value->set_method(record.method_);
        value->set_host(record.host_);
        value->set_target(record.target_);
        value->set_status_code(record.status_code_);
        value->set_request_bytes(record.request_bytes_);
        value->set_response_bytes(record.response_bytes_);
        value->set_duration_ms(record.duration_ms_);
        value->set_user_agent(record.user_agent_);
        value->set_referer(record.referer_);
        value->set_request_headers(record.request_headers_);
        value->set_request_body(record.request_body_);
        value->set_request_body_truncated(record.request_body_truncated_);
        value->set_tls_fingerprint(record.tls_fingerprint_);
        value->set_response_headers(record.response_headers_);
        value->set_query_string(record.query_string_);
        value->set_cookies(record.cookies_);
        return event;
    }

    static v2::LogEvent node_event(std::string_view level, std::string_view category,
        std::string_view message) {
        v2::LogEvent event;
        auto* value = event.mutable_node_log();
        value->set_id(uuid());
        value->set_occurred_unix_ms(now_ms());
        value->set_level(bounded(level, log_contract::max_node_level_bytes));
        value->set_category(bounded(category, log_contract::max_node_category_bytes));
        value->set_message(bounded(message, log_contract::max_node_message_bytes));
        return event;
    }

    std::shared_ptr<producer_state> state_;
    ruvia::event_loop_stop_registration stop_registration_;
};

}  // namespace flexedge::node
