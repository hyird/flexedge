#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <openssl/rand.h>

#include "node/proto/edge_control.pb.h"
#include "node/proto/log_contract.h"

namespace flexedge::node {

class NodeLogBuffer final {
  private:
    struct BufferedEvent final {
        v2::LogEvent value;
        std::uint64_t serializedBytes{};
    };

  public:
    struct AccessRecord final {
        std::string websiteId;
        std::string clientIp;
        std::string protocol;
        std::string method;
        std::string host;
        std::string target;
        std::uint32_t statusCode{};
        std::uint64_t responseBytes{};
        std::uint64_t durationMs{};
        std::string userAgent;
        std::string referer;
        std::string requestHeaders;
        std::string requestBody;
        bool requestBodyTruncated{};
        std::string tlsFingerprint;
        std::string responseHeaders;
        std::string queryString;
        std::string cookies;
    };

    class Producer final {
      public:
        bool access(AccessRecord record) const {
            return owner_->push(shardIndex_, accessEvent(std::move(record)));
        }

        bool node(std::string_view level, std::string_view category,
                  std::string_view message) const {
            return owner_->push(shardIndex_, nodeEvent(level, category, message));
        }

      private:
        friend class NodeLogBuffer;

        Producer(NodeLogBuffer& owner, std::size_t shardIndex)
            : owner_(&owner), shardIndex_(shardIndex) {}

        NodeLogBuffer* owner_;
        std::size_t shardIndex_;
    };

    class InFlightDelivery final {
      public:
        InFlightDelivery(InFlightDelivery&&) noexcept = default;
        InFlightDelivery& operator=(InFlightDelivery&&) noexcept = default;
        InFlightDelivery(const InFlightDelivery&) = delete;
        InFlightDelivery& operator=(const InFlightDelivery&) = delete;

        [[nodiscard]] const v2::LogDelivery& value() const noexcept { return value_; }

      private:
        friend class NodeLogBuffer;

        struct Contribution final {
            std::size_t shardIndex{};
            std::uint64_t eventCount{};
            std::uint64_t retainedBytes{};
        };

        InFlightDelivery() = default;

        v2::LogDelivery value_;
        std::vector<Contribution> contributions_;
        std::vector<std::uint64_t> serializedBytes_;
    };

    static constexpr std::size_t kMaxRequestHeadersBytes{log_contract::kMaxRequestHeadersBytes};
    static constexpr std::size_t kMaxResponseHeadersBytes{log_contract::kMaxResponseHeadersBytes};
    static constexpr std::size_t kMaxRequestBodyBytes{log_contract::kMaxRequestBodyBytes};
    static constexpr std::size_t kMaxQueryStringBytes{log_contract::kMaxQueryStringBytes};
    static constexpr std::size_t kMaxCookiesBytes{log_contract::kMaxCookiesBytes};

    explicit NodeLogBuffer(std::size_t workerShardCount = 0) {
        shards_.reserve(workerShardCount == 0 ? 1 : workerShardCount + 2);
        if (workerShardCount == 0) {
            shards_.push_back(std::make_unique<Shard>(kMaxPendingEvents, kMaxPendingBytes));
            return;
        }
        shards_.push_back(std::make_unique<Shard>(kControlPendingEvents, kControlPendingBytes));
        shards_.push_back(std::make_unique<Shard>(kControlPendingEvents, kControlPendingBytes));
        workerShardOffset_ = 2;
        for (std::size_t index = 0; index < workerShardCount; ++index) {
            const auto events = kMaxPendingEvents / workerShardCount +
                                (index < kMaxPendingEvents % workerShardCount ? 1 : 0);
            const auto bytes = kMaxPendingBytes / workerShardCount +
                               (index < kMaxPendingBytes % workerShardCount ? 1 : 0);
            shards_.push_back(std::make_unique<Shard>(events, bytes));
        }
    }

    NodeLogBuffer(const NodeLogBuffer&) = delete;
    NodeLogBuffer& operator=(const NodeLogBuffer&) = delete;

    [[nodiscard]] Producer workerProducer(std::size_t workerIndex) {
        if (workerShardOffset_ == 0 || workerShardOffset_ + workerIndex >= shards_.size()) {
            throw std::out_of_range("log producer worker index is out of range");
        }
        return Producer(*this, workerShardOffset_ + workerIndex);
    }

    [[nodiscard]] Producer backgroundProducer() {
        return Producer(*this, workerShardOffset_ == 0 ? 0 : 1);
    }

    static std::string requestHeaderValue(std::string_view name, std::string_view value) {
        (void)name;
        return std::string(value);
    }

    static std::string requestBodyValue(std::string_view value) { return std::string(value); }

    static std::string responseHeaderValue(std::string_view name, std::string_view value) {
        return requestHeaderValue(name, value);
    }

    static std::string cookieValue(std::string_view value) { return std::string(value); }

    bool access(AccessRecord record) { return push(0, accessEvent(std::move(record))); }

    bool node(std::string_view level, std::string_view category, std::string_view message) {
        return push(0, nodeEvent(level, category, message));
    }

    [[nodiscard]] bool pending() const noexcept { return queuedEvents() != 0; }

    [[nodiscard]] std::uint64_t queuedEvents() const noexcept {
        std::uint64_t result{};
        for (const auto& shard : shards_) {
            result += shard->queuedEvents.load(std::memory_order_relaxed);
        }
        return result;
    }

    [[nodiscard]] std::uint64_t retainedEvents() const noexcept {
        std::uint64_t result{};
        for (const auto& shard : shards_) {
            result += shard->retainedEvents.load(std::memory_order_acquire);
        }
        return result;
    }

    [[nodiscard]] std::uint64_t droppedEvents() const noexcept {
        std::uint64_t result{};
        for (const auto& shard : shards_) {
            result += shard->droppedEvents.load(std::memory_order_relaxed);
        }
        return result;
    }

    [[nodiscard]] bool waitForControlDrain(std::chrono::milliseconds timeout) const noexcept {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        const auto controlShardCount =
            workerShardOffset_ == 0 ? std::size_t{1} : workerShardOffset_;
        const auto controlPending = [this, controlShardCount] {
            for (std::size_t index = 0; index < controlShardCount; ++index) {
                if (shards_[index]->retainedEvents.load(std::memory_order_acquire) != 0) {
                    return true;
                }
            }
            return false;
        };
        while (controlPending()) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return true;
    }

    [[nodiscard]] bool waitForDrain(std::chrono::milliseconds timeout) const noexcept {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (retainedEvents() != 0) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return true;
    }

    [[nodiscard]] std::optional<InFlightDelivery> take(std::string_view nodeId) {
        if (deliveryInFlight_ || !pending()) {
            return std::nullopt;
        }
        InFlightDelivery delivery;
        delivery.value_.set_node_id(std::string(nodeId));
        delivery.contributions_.reserve(shards_.size());
        delivery.value_.mutable_events()->Reserve(kMaxDeliveryEvents);
        delivery.serializedBytes_.reserve(kMaxDeliveryEvents);
        std::size_t wireBytes = fieldWireBytes(nodeId.size());

        bool deliveryFull{};
        if (workerShardOffset_ != 0) {
            // Keep control and self-upgrade logs observable even while access logs are saturated.
            for (std::size_t index = 0; index < workerShardOffset_; ++index) {
                if (appendShard(delivery, wireBytes, index)) {
                    deliveryFull = true;
                    break;
                }
            }
        }
        if (!deliveryFull) {
            const auto firstDataShard =
                workerShardOffset_ == 0 ? std::size_t{0} : workerShardOffset_;
            const auto dataShardCount = shards_.size() - firstDataShard;
            const auto start = nextShard_ % dataShardCount;
            std::size_t visited{};
            for (;
                 visited < dataShardCount && delivery.serializedBytes_.size() < kMaxDeliveryEvents;
                 ++visited) {
                const auto shardIndex = firstDataShard + (start + visited) % dataShardCount;
                if (appendShard(delivery, wireBytes, shardIndex)) {
                    ++visited;
                    break;
                }
            }
            nextShard_ = (start + visited) % dataShardCount;
        }
        if (delivery.serializedBytes_.empty()) {
            return std::nullopt;
        }
        deliveryInFlight_ = true;
        return delivery;
    }

    void acknowledge(InFlightDelivery delivery) noexcept {
        for (const auto& contribution : delivery.contributions_) {
            auto& shard = *shards_[contribution.shardIndex];
            shard.discard(static_cast<std::size_t>(contribution.eventCount));
            shard.queuedEvents.fetch_sub(contribution.eventCount, std::memory_order_relaxed);
            shard.retainedEvents.fetch_sub(contribution.eventCount, std::memory_order_relaxed);
            shard.retainedBytes.fetch_sub(contribution.retainedBytes, std::memory_order_relaxed);
        }
        deliveryInFlight_ = false;
    }

    void restore(InFlightDelivery delivery) noexcept {
        if (!delivery.contributions_.empty()) {
            nextShard_ = delivery.contributions_.front().shardIndex;
        }
        deliveryInFlight_ = false;
    }

  private:
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
    struct alignas(64) Shard final {
        Shard(std::uint64_t maximumEvents, std::uint64_t maximumBytes)
            : events(static_cast<std::size_t>(maximumEvents) + 1), maximumEvents(maximumEvents),
              maximumBytes(maximumBytes) {}

        [[nodiscard]] bool push(BufferedEvent event) {
            const auto next = advance(writeIndex_);
            if (next == readIndex_.load(std::memory_order_acquire)) {
                return false;
            }
            events[writeIndex_].emplace(std::move(event));
            writeIndex_ = next;
            publishedWriteIndex_.store(writeIndex_, std::memory_order_release);
            return true;
        }

        [[nodiscard]] const BufferedEvent* peek(std::size_t offset) const noexcept {
            const auto read = readIndex_.load(std::memory_order_relaxed);
            const auto published = publishedWriteIndex_.load(std::memory_order_acquire);
            if (offset >= distance(read, published)) {
                return nullptr;
            }
            const auto& event = events[advance(read, offset)];
            return event ? std::addressof(*event) : nullptr;
        }

        void discard(std::size_t count) noexcept {
            auto read = readIndex_.load(std::memory_order_relaxed);
            for (std::size_t index = 0; index < count; ++index) {
                events[read].reset();
                read = advance(read);
            }
            readIndex_.store(read, std::memory_order_release);
        }

        [[nodiscard]] std::size_t advance(std::size_t index,
                                          std::size_t amount = 1) const noexcept {
            return (index + amount) % events.size();
        }

        [[nodiscard]] std::size_t distance(std::size_t first, std::size_t last) const noexcept {
            return last >= first ? last - first : events.size() - first + last;
        }

        std::vector<std::optional<BufferedEvent>> events;
        // The worker that owns this shard is the sole writer; the control loop is the sole reader.
        std::size_t writeIndex_{};
        std::atomic<std::size_t> publishedWriteIndex_{};
        std::atomic<std::size_t> readIndex_{};
        std::atomic<std::uint64_t> queuedEvents{};
        std::atomic<std::uint64_t> retainedEvents{};
        std::atomic<std::uint64_t> retainedBytes{};
        std::atomic<std::uint64_t> droppedEvents{};
        std::uint64_t maximumEvents;
        std::uint64_t maximumBytes;
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

    static constexpr std::size_t kMaxPendingEvents{8192};
    static constexpr std::size_t kMaxPendingBytes{64 * 1024 * 1024};
    static constexpr std::size_t kControlPendingEvents{512};
    static constexpr std::size_t kControlPendingBytes{4 * 1024 * 1024};
    static constexpr int kMaxDeliveryEvents{static_cast<int>(log_contract::kMaxDeliveryEvents)};
    static constexpr std::size_t kMaxDeliveryBytes{log_contract::kMaxDeliveryBytes};
    static constexpr std::size_t kMaxEventBytes{kMaxDeliveryBytes - 128};

    static std::size_t varintBytes(std::size_t value) noexcept {
        std::size_t result{1};
        while (value >= 0x80) {
            value >>= 7;
            ++result;
        }
        return result;
    }

    static std::size_t fieldWireBytes(std::size_t valueBytes) noexcept {
        return 1 + varintBytes(valueBytes) + valueBytes;
    }

    [[nodiscard]] bool appendShard(InFlightDelivery& delivery, std::size_t& wireBytes,
                                   std::size_t shardIndex) {
        auto& shard = *shards_[shardIndex];
        std::size_t offset{};
        std::uint64_t retainedBytes{};
        while (delivery.serializedBytes_.size() < kMaxDeliveryEvents) {
            const auto* event = shard.peek(offset);
            if (event == nullptr) {
                break;
            }
            const auto eventBytes = event->serializedBytes;
            const auto eventWireBytes = fieldWireBytes(eventBytes);
            if (wireBytes + eventWireBytes > kMaxDeliveryBytes) {
                break;
            }
            wireBytes += eventWireBytes;
            retainedBytes += eventBytes;
            *delivery.value_.add_events() = event->value;
            delivery.serializedBytes_.push_back(eventBytes);
            ++offset;
        }
        if (offset != 0) {
            delivery.contributions_.push_back(
                {.shardIndex = shardIndex, .eventCount = offset, .retainedBytes = retainedBytes});
        }
        return wireBytes >= kMaxDeliveryBytes || (shard.peek(offset) != nullptr && offset != 0);
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

    [[nodiscard]] bool push(std::size_t shardIndex, v2::LogEvent event) {
        auto& shard = *shards_[shardIndex];
        const auto eventBytes = static_cast<std::uint64_t>(event.ByteSizeLong());
        if (eventBytes > kMaxEventBytes || !reserve(shard.retainedEvents, 1, shard.maximumEvents)) {
            shard.droppedEvents.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (!reserve(shard.retainedBytes, eventBytes, shard.maximumBytes)) {
            shard.retainedEvents.fetch_sub(1, std::memory_order_relaxed);
            shard.droppedEvents.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        try {
            if (!shard.push({.value = std::move(event), .serializedBytes = eventBytes})) {
                shard.retainedBytes.fetch_sub(eventBytes, std::memory_order_relaxed);
                shard.retainedEvents.fetch_sub(1, std::memory_order_relaxed);
                shard.droppedEvents.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            shard.queuedEvents.fetch_add(1, std::memory_order_relaxed);
        } catch (...) {
            shard.retainedBytes.fetch_sub(eventBytes, std::memory_order_relaxed);
            shard.retainedEvents.fetch_sub(1, std::memory_order_relaxed);
            throw;
        }
        return true;
    }

    static std::uint64_t nowMs() {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::chrono::system_clock::now().time_since_epoch())
                                              .count());
    }

    static std::string uuid() {
        unsigned char bytes[16]{};
        if (RAND_bytes(bytes, sizeof(bytes)) != 1) {
            const auto value = nowMs();
            for (std::size_t index = 0; index < sizeof(bytes); ++index) {
                bytes[index] = static_cast<unsigned char>((value >> ((index % 8) * 8)) & 0xff);
            }
        }
        bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0f) | 0x40);
        bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3f) | 0x80);
        constexpr char kHex[] = "0123456789abcdef";
        std::string result;
        result.reserve(36);
        for (std::size_t index = 0; index < sizeof(bytes); ++index) {
            if (index == 4 || index == 6 || index == 8 || index == 10) {
                result.push_back('-');
            }
            result.push_back(kHex[bytes[index] >> 4]);
            result.push_back(kHex[bytes[index] & 0x0f]);
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

    static AccessRecord clamp(AccessRecord record) {
        record.websiteId = bounded(record.websiteId, log_contract::kMaxWebsiteIdBytes);
        record.clientIp = bounded(record.clientIp, log_contract::kMaxClientIpBytes);
        record.protocol = bounded(record.protocol, log_contract::kMaxProtocolBytes);
        record.method = bounded(record.method, log_contract::kMaxMethodBytes);
        record.host = bounded(record.host, log_contract::kMaxHostBytes);
        record.target = bounded(record.target, log_contract::kMaxTargetBytes);
        record.statusCode = std::clamp(record.statusCode, 100u, 999u);
        record.responseBytes = (std::min)(record.responseBytes, log_contract::kMaxResponseBytes);
        record.durationMs = (std::min)(record.durationMs, log_contract::kMaxDurationMs);
        record.userAgent = bounded(record.userAgent, log_contract::kMaxUserAgentBytes);
        record.referer = bounded(record.referer, log_contract::kMaxRefererBytes);
        record.requestBodyTruncated =
            record.requestBodyTruncated || record.requestBody.size() > kMaxRequestBodyBytes;
        record.requestHeaders = printable(record.requestHeaders, kMaxRequestHeadersBytes);
        record.responseHeaders = printable(record.responseHeaders, kMaxResponseHeadersBytes);
        record.requestBody = printable(record.requestBody, kMaxRequestBodyBytes);
        record.tlsFingerprint =
            bounded(record.tlsFingerprint, log_contract::kMaxTlsFingerprintBytes);
        record.queryString = printable(record.queryString, kMaxQueryStringBytes);
        record.cookies = printable(record.cookies, kMaxCookiesBytes);
        return record;
    }

    static v2::LogEvent accessEvent(AccessRecord record) {
        record = clamp(std::move(record));
        v2::LogEvent event;
        auto* value = event.mutable_access_log();
        value->set_id(uuid());
        value->set_occurred_unix_ms(nowMs());
        value->set_website_id(record.websiteId);
        value->set_client_ip(record.clientIp);
        value->set_protocol(record.protocol);
        value->set_method(record.method);
        value->set_host(record.host);
        value->set_target(record.target);
        value->set_status_code(record.statusCode);
        value->set_response_bytes(record.responseBytes);
        value->set_duration_ms(record.durationMs);
        value->set_user_agent(record.userAgent);
        value->set_referer(record.referer);
        value->set_request_headers(record.requestHeaders);
        value->set_request_body(record.requestBody);
        value->set_request_body_truncated(record.requestBodyTruncated);
        value->set_tls_fingerprint(record.tlsFingerprint);
        value->set_response_headers(record.responseHeaders);
        value->set_query_string(record.queryString);
        value->set_cookies(record.cookies);
        return event;
    }

    static v2::LogEvent nodeEvent(std::string_view level, std::string_view category,
                                  std::string_view message) {
        v2::LogEvent event;
        auto* value = event.mutable_node_log();
        value->set_id(uuid());
        value->set_occurred_unix_ms(nowMs());
        value->set_level(bounded(level, log_contract::kMaxNodeLevelBytes));
        value->set_category(bounded(category, log_contract::kMaxNodeCategoryBytes));
        value->set_message(bounded(message, log_contract::kMaxNodeMessageBytes));
        return event;
    }

    std::vector<std::unique_ptr<Shard>> shards_;
    std::size_t workerShardOffset_{};
    std::size_t nextShard_{};
    bool deliveryInFlight_{};
};

} // namespace flexedge::node
