#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <openssl/rand.h>

#include "node/proto/edge_control.pb.h"
#include "node/proto/log_contract.h"

namespace flexedge::node {

class node_log_buffer final {
private:
    struct buffered_event final {
        v2::LogEvent value_;
        std::uint64_t serialized_bytes_{};
    };

public:
    struct access_record final {
        std::string website_id_;
        std::string client_ip_;
        std::string protocol_;
        std::string method_;
        std::string host_;
        std::string target_;
        std::uint32_t status_code_{};
        std::uint64_t request_bytes_{};
        std::uint64_t response_bytes_{};
        std::uint64_t duration_ms_{};
        std::string user_agent_;
        std::string referer_;
        std::string request_headers_;
        std::string request_body_;
        bool request_body_truncated_{};
        std::string tls_fingerprint_;
        std::string response_headers_;
        std::string query_string_;
        std::string cookies_;
    };

    class producer final {
    public:
        bool access(access_record record) const {
            return owner_->push(shard_index_, access_event(std::move(record)));
        }

        bool node(std::string_view level, std::string_view category,
            std::string_view message) const {
            return owner_->push(shard_index_, node_event(level, category, message));
        }

    private:
        friend class node_log_buffer;

        producer(node_log_buffer& owner, std::size_t shard_index)
            : owner_(&owner),
              shard_index_(shard_index) {}

        node_log_buffer* owner_;
        std::size_t shard_index_;
    };

    class in_flight_delivery final {
    public:
        in_flight_delivery(in_flight_delivery&&) noexcept = default;
        in_flight_delivery& operator=(in_flight_delivery&&) noexcept = default;
        in_flight_delivery(const in_flight_delivery&) = delete;
        in_flight_delivery& operator=(const in_flight_delivery&) = delete;

        [[nodiscard]] const v2::LogDelivery& value() const noexcept {
            return value_;
        }

    private:
        friend class node_log_buffer;

        struct contribution final {
            std::size_t shard_index_{};
            std::uint64_t event_count_{};
            std::uint64_t retained_bytes_{};
        };

        in_flight_delivery() = default;

        v2::LogDelivery value_;
        std::vector<contribution> contributions_;
        std::vector<std::uint64_t> serialized_bytes_;
    };

    static constexpr std::size_t max_request_headers_bytes{log_contract::max_request_headers_bytes};
    static constexpr std::size_t max_response_headers_bytes{log_contract::max_response_headers_bytes};
    static constexpr std::size_t max_request_body_bytes{log_contract::max_request_body_bytes};
    static constexpr std::size_t max_query_string_bytes{log_contract::max_query_string_bytes};
    static constexpr std::size_t max_cookies_bytes{log_contract::max_cookies_bytes};

    explicit node_log_buffer(std::size_t worker_shard_count = 0,
        std::function<void()> on_queued = {})
        : on_queued_(std::move(on_queued)) {
        shards_.reserve(worker_shard_count == 0 ? 1 : worker_shard_count + 2);
        if (worker_shard_count == 0) {
            shards_.push_back(std::make_unique<shard>(max_pending_events, max_pending_bytes));
            return;
        }
        shards_.push_back(std::make_unique<shard>(control_pending_events, control_pending_bytes));
        shards_.push_back(std::make_unique<shard>(control_pending_events, control_pending_bytes));
        worker_shard_offset_ = 2;
        for (std::size_t index = 0; index < worker_shard_count; ++index) {
            const auto events = max_pending_events / worker_shard_count +
                                (index < max_pending_events % worker_shard_count ? 1 : 0);
            const auto bytes = max_pending_bytes / worker_shard_count +
                               (index < max_pending_bytes % worker_shard_count ? 1 : 0);
            shards_.push_back(std::make_unique<shard>(events, bytes));
        }
    }

    node_log_buffer(const node_log_buffer&) = delete;
    node_log_buffer& operator=(const node_log_buffer&) = delete;

    [[nodiscard]] producer worker_producer(std::size_t worker_index) {
        if (worker_shard_offset_ == 0 || worker_shard_offset_ + worker_index >= shards_.size()) {
            throw std::out_of_range("log producer worker index is out of range");
        }
        return producer(*this, worker_shard_offset_ + worker_index);
    }

    [[nodiscard]] producer background_producer() {
        return producer(*this, worker_shard_offset_ == 0 ? 0 : 1);
    }

    bool access(access_record record) {
        return push(0, access_event(std::move(record)));
    }

    bool node(std::string_view level, std::string_view category, std::string_view message) {
        return push(0, node_event(level, category, message));
    }

    [[nodiscard]] bool pending() const noexcept {
        return queued_events() != 0;
    }

    [[nodiscard]] std::uint64_t queued_events() const noexcept {
        std::uint64_t result{};
        for (const auto& shard : shards_) {
            result += shard->queued_events_.load(std::memory_order_relaxed);
        }
        return result;
    }

    [[nodiscard]] std::uint64_t retained_events() const noexcept {
        std::uint64_t result{};
        for (const auto& shard : shards_) {
            result += shard->retained_events_.load(std::memory_order_acquire);
        }
        return result;
    }

    [[nodiscard]] std::uint64_t dropped_events() const noexcept {
        std::uint64_t result{};
        for (const auto& shard : shards_) {
            result += shard->dropped_events_.load(std::memory_order_relaxed);
        }
        return result;
    }

    [[nodiscard]] std::optional<in_flight_delivery> take(std::string_view node_id) {
        if (delivery_in_flight_ || !pending()) {
            return std::nullopt;
        }
        in_flight_delivery delivery;
        delivery.value_.set_node_id(std::string(node_id));
        delivery.contributions_.reserve(shards_.size());
        delivery.value_.mutable_events()->Reserve(max_delivery_events);
        delivery.serialized_bytes_.reserve(max_delivery_events);
        std::size_t wire_bytes = field_wire_bytes(node_id.size());

        bool delivery_full{};
        if (worker_shard_offset_ != 0) {
            // Keep control and self-upgrade logs observable even while access logs are saturated.
            for (std::size_t index = 0; index < worker_shard_offset_; ++index) {
                if (append_shard(delivery, wire_bytes, index)) {
                    delivery_full = true;
                    break;
                }
            }
        }
        if (!delivery_full) {
            const auto first_data_shard =
                worker_shard_offset_ == 0 ? std::size_t{0} : worker_shard_offset_;
            const auto data_shard_count = shards_.size() - first_data_shard;
            const auto start = next_shard_ % data_shard_count;
            std::size_t visited{};
            for (;
                visited < data_shard_count && delivery.serialized_bytes_.size() < max_delivery_events;
                ++visited) {
                const auto shard_index = first_data_shard + (start + visited) % data_shard_count;
                if (append_shard(delivery, wire_bytes, shard_index)) {
                    ++visited;
                    break;
                }
            }
            next_shard_ = (start + visited) % data_shard_count;
        }
        if (delivery.serialized_bytes_.empty()) {
            return std::nullopt;
        }
        delivery_in_flight_ = true;
        return delivery;
    }

    void acknowledge(in_flight_delivery delivery) noexcept {
        for (const auto& contribution : delivery.contributions_) {
            auto& shard = *shards_[contribution.shard_index_];
            shard.discard(static_cast<std::size_t>(contribution.event_count_));
            shard.queued_events_.fetch_sub(contribution.event_count_, std::memory_order_relaxed);
            shard.retained_events_.fetch_sub(contribution.event_count_, std::memory_order_relaxed);
            shard.retained_bytes_.fetch_sub(contribution.retained_bytes_, std::memory_order_relaxed);
        }
        delivery_in_flight_ = false;
    }

    void restore(in_flight_delivery delivery) noexcept {
        if (!delivery.contributions_.empty()) {
            next_shard_ = delivery.contributions_.front().shard_index_;
        }
        delivery_in_flight_ = false;
    }

private:
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
    struct alignas(64) shard final {
        shard(std::uint64_t maximum_events, std::uint64_t maximum_bytes)
            : events_(static_cast<std::size_t>(maximum_events) + 1),
              maximum_events_(maximum_events),
              maximum_bytes_(maximum_bytes) {}

        [[nodiscard]] bool push(buffered_event event) {
            const auto next = advance(write_index_);
            if (next == read_index_.load(std::memory_order_acquire)) {
                return false;
            }
            events_[write_index_].emplace(std::move(event));
            write_index_ = next;
            published_write_index_.store(write_index_, std::memory_order_release);
            return true;
        }

        [[nodiscard]] const buffered_event* peek(std::size_t offset) const noexcept {
            const auto read = read_index_.load(std::memory_order_relaxed);
            const auto published = published_write_index_.load(std::memory_order_acquire);
            if (offset >= distance(read, published)) {
                return nullptr;
            }
            const auto& event = events_[advance(read, offset)];
            return event ? std::addressof(*event) : nullptr;
        }

        void discard(std::size_t count) noexcept {
            auto read = read_index_.load(std::memory_order_relaxed);
            for (std::size_t index = 0; index < count; ++index) {
                events_[read].reset();
                read = advance(read);
            }
            read_index_.store(read, std::memory_order_release);
        }

        [[nodiscard]] std::size_t advance(std::size_t index,
            std::size_t amount = 1) const noexcept {
            return (index + amount) % events_.size();
        }

        [[nodiscard]] std::size_t distance(std::size_t first, std::size_t last) const noexcept {
            return last >= first ? last - first : events_.size() - first + last;
        }

        std::vector<std::optional<buffered_event>> events_;
        // The worker that owns this shard is the sole writer; the control loop is the sole reader.
        std::size_t write_index_{};
        std::atomic<std::size_t> published_write_index_{};
        std::atomic<std::size_t> read_index_{};
        std::atomic<std::uint64_t> queued_events_{};
        std::atomic<std::uint64_t> retained_events_{};
        std::atomic<std::uint64_t> retained_bytes_{};
        std::atomic<std::uint64_t> dropped_events_{};
        std::uint64_t maximum_events_;
        std::uint64_t maximum_bytes_;
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

    static constexpr std::size_t max_pending_events{8192};
    static constexpr std::size_t max_pending_bytes{64 * 1024 * 1024};
    static constexpr std::size_t control_pending_events{512};
    static constexpr std::size_t control_pending_bytes{4 * 1024 * 1024};
    static constexpr int max_delivery_events{static_cast<int>(log_contract::max_delivery_events)};
    static constexpr std::size_t max_delivery_bytes{log_contract::max_delivery_bytes};
    static constexpr std::size_t max_event_bytes{max_delivery_bytes - 128};

    static std::size_t varint_bytes(std::size_t value) noexcept {
        std::size_t result{1};
        while (value >= 0x80) {
            value >>= 7;
            ++result;
        }
        return result;
    }

    static std::size_t field_wire_bytes(std::size_t value_bytes) noexcept {
        return 1 + varint_bytes(value_bytes) + value_bytes;
    }

    [[nodiscard]] bool append_shard(in_flight_delivery& delivery, std::size_t& wire_bytes,
        std::size_t shard_index) {
        auto& shard = *shards_[shard_index];
        std::size_t offset{};
        std::uint64_t retained_bytes{};
        while (delivery.serialized_bytes_.size() < max_delivery_events) {
            const auto* event = shard.peek(offset);
            if (event == nullptr) {
                break;
            }
            const auto event_bytes = event->serialized_bytes_;
            const auto event_wire_bytes = field_wire_bytes(event_bytes);
            if (wire_bytes + event_wire_bytes > max_delivery_bytes) {
                break;
            }
            wire_bytes += event_wire_bytes;
            retained_bytes += event_bytes;
            *delivery.value_.add_events() = event->value_;
            delivery.serialized_bytes_.push_back(event_bytes);
            ++offset;
        }
        if (offset != 0) {
            delivery.contributions_.push_back(
                {.shard_index_ = shard_index, .event_count_ = offset, .retained_bytes_ = retained_bytes});
        }
        return wire_bytes >= max_delivery_bytes || (shard.peek(offset) != nullptr && offset != 0);
    }

    static bool reserve(std::atomic<std::uint64_t>& counter, std::uint64_t amount,
        std::uint64_t maximum) noexcept {
        auto current = counter.load(std::memory_order_relaxed);
        for (;;) {
            if (amount > maximum || current > maximum - amount) {
                return false;
            }
            if (counter.compare_exchange_weak(current, current + amount,
                    std::memory_order_relaxed)) {
                return true;
            }
        }
    }

    [[nodiscard]] bool push(std::size_t shard_index, v2::LogEvent event) {
        auto& shard = *shards_[shard_index];
        const auto event_bytes = static_cast<std::uint64_t>(event.ByteSizeLong());
        if (event_bytes > max_event_bytes || !reserve(shard.retained_events_, 1, shard.maximum_events_)) {
            shard.dropped_events_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (!reserve(shard.retained_bytes_, event_bytes, shard.maximum_bytes_)) {
            shard.retained_events_.fetch_sub(1, std::memory_order_relaxed);
            shard.dropped_events_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        try {
            if (!shard.push({.value_ = std::move(event), .serialized_bytes_ = event_bytes})) {
                shard.retained_bytes_.fetch_sub(event_bytes, std::memory_order_relaxed);
                shard.retained_events_.fetch_sub(1, std::memory_order_relaxed);
                shard.dropped_events_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            shard.queued_events_.fetch_add(1, std::memory_order_relaxed);
        } catch (...) {
            shard.retained_bytes_.fetch_sub(event_bytes, std::memory_order_relaxed);
            shard.retained_events_.fetch_sub(1, std::memory_order_relaxed);
            throw;
        }
        if (on_queued_) {
            on_queued_();
        }
        return true;
    }

    // Immutable after construction; producers may notify concurrently.
    std::function<void()> on_queued_;

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

    std::vector<std::unique_ptr<shard>> shards_;
    std::size_t worker_shard_offset_{};
    std::size_t next_shard_{};
    bool delivery_in_flight_{};
};

}  // namespace flexedge::node
