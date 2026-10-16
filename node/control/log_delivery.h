#pragma once

#include <ruvia/core/channel.h>
#include <ruvia/core/stop_token.h>
#include <ruvia/core/task_scope.h>
#include <ruvia/core/timer.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "node/control/channel_log.h"
#include "node/control/control_stream.h"
#include "node/runtime/log_producer.h"

namespace flexedge::node {

// Receive queues, capacity accounting, retry and ACK state belong exclusively
// to the control worker. Receiving continues while the WebSocket reconnects.
class log_delivery final {
public:
    log_delivery(log_producer& logs, ruvia::channel_receiver<owned_log_batch> data,
        ruvia::channel_receiver<owned_log_batch> control)
        : data_(std::move(data), 8192, 64 * 1024 * 1024),
          control_(std::move(control), 1024, 8 * 1024 * 1024),
          channel_log_(logs, "log") {
        auto [sender, receiver] = ruvia::make_channel<bool>(data_.receiver_.worker(), {.capacity_ = 1});
        ready_sender_ = std::move(sender);
        ready_receiver_.emplace(std::move(receiver));
    }

    log_delivery(const log_delivery&) = delete;
    log_delivery& operator=(const log_delivery&) = delete;

    // The control runtime owns this task independently of authenticated sessions.
    ruvia::task<void> receive_batches(ruvia::stop_token stop) {
        const auto worker = data_.receiver_.worker();
        ruvia::task_scope receivers(worker);
        const auto receiving_stop = ruvia::combine_stop_tokens(std::move(stop), receivers.get_stop_token());
        std::exception_ptr failure;
        try {
            receivers.spawn(receive_source(data_, receiving_stop));
            receivers.spawn(receive_source(control_, receiving_stop));
        } catch (...) {
            failure = std::current_exception();
            receivers.request_stop();
        }
        try {
            co_await receivers.join();
        } catch (...) {
            if (!failure) {
                failure = std::current_exception();
            }
        }
        if (failure) {
            std::rethrow_exception(failure);
        }
    }

    // A session owns only sending. Unacknowledged content stays in this object
    // unchanged for the next authenticated session; producers never receive ACKs.
    ruvia::task<void> run(control_stream& stream, std::string node_id) {
        const auto stop = stream.get_stop_token();
        while (!stop.stop_requested()) {
            if (!in_flight_) {
                auto* source = !control_.pending_.empty() ? &control_ : !data_.pending_.empty() ? &data_
                                                                                                : nullptr;
                if (!source) {
                    if (data_.closed_ && control_.closed_) {
                        co_return;
                    }
                    if (!(co_await ready_receiver_->receive(stop)).has_value()) {
                        co_return;
                    }
                    continue;
                }
                prepare_delivery(*source, node_id);
            }
            bool retry{};
            try {
                v2::ClientEnvelope envelope;
                *envelope.mutable_log_delivery() = in_flight_->value_;
                auto request = stream.send(envelope, control_request_priority::logs);
                const auto acknowledgement = co_await request.receive(std::chrono::seconds(5));
                if (!acknowledgement.has_log_delivery_ack()) {
                    throw std::runtime_error("control plane did not acknowledge log delivery");
                }
                auto& source = *in_flight_->source_;
                source.retained_events_ -= in_flight_->event_count_;
                source.retained_bytes_ -= in_flight_->event_bytes_;
                source.acknowledged_events_ += in_flight_->event_count_;
                in_flight_.reset();
                (void)source.space_sender_.send(true);
                channel_log_.recovered();
            } catch (const control_request_error& error) {
                if (stop.stop_requested()) {
                    co_return;
                }
                channel_log_.failed(error.what(), error.retryable() ? channel_failure_kind::transient : channel_failure_kind::permanent);
                retry = true;
            } catch (const ruvia::websocket_client_error& error) {
                if (stop.stop_requested()) {
                    co_return;
                }
                channel_log_.failed(error);
                retry = true;
            } catch (const std::exception& error) {
                if (stop.stop_requested()) {
                    co_return;
                }
                channel_log_.failed(error.what(), channel_failure_kind::permanent);
                retry = true;
            }
            if (retry && co_await ruvia::sleep_for(stream.worker(), channel_log_.retry_delay(), stop) == ruvia::timer_sleep_result::stop_requested) {
                co_return;
            }
        }
    }

    [[nodiscard]] std::uint64_t queued_events(const log_production_snapshot& data,
        const log_production_snapshot& control) const {
        if (!data_.receiver_.worker().is_current()) {
            throw std::logic_error("log delivery statistics belong to the control worker");
        }
        const auto outstanding = [](const batch_source& source, const log_production_snapshot& production) {
            // Worker reports and ACKs may describe different instants. The
            // control worker's cumulative receipts are an authoritative lower
            // bound, and one source's ACKs must never cancel another's backlog.
            const auto published = (std::max)(production.published_events_, source.received_events_);
            return published - source.acknowledged_events_ + production.queued_events_;
        };
        return outstanding(data_, data) + outstanding(control_, control);
    }

private:
    struct queued_batch final {
        owned_log_batch value_;
        int next_event_{};
    };

    struct batch_source final {
        batch_source(ruvia::channel_receiver<owned_log_batch> receiver, std::size_t events, std::size_t bytes)
            : receiver_(std::move(receiver)),
              maximum_events_(events),
              maximum_bytes_(bytes) {
            auto [sender, space] = ruvia::make_channel<bool>(receiver_.worker(), {.capacity_ = 1});
            space_sender_ = std::move(sender);
            space_receiver_.emplace(std::move(space));
        }

        ruvia::channel_receiver<owned_log_batch> receiver_;
        std::deque<queued_batch> pending_;
        owned_log_batch waiting_;
        ruvia::channel_sender<bool> space_sender_;
        std::optional<ruvia::channel_receiver<bool>> space_receiver_;
        std::size_t maximum_events_;
        std::size_t maximum_bytes_;
        std::size_t retained_events_{};
        std::size_t retained_bytes_{};
        std::uint64_t received_events_{};
        std::uint64_t acknowledged_events_{};
        bool closed_{};
    };

    struct in_flight_delivery final {
        v2::LogDelivery value_{};
        batch_source* source_{};
        std::size_t event_count_{};
        std::size_t event_bytes_{};
    };

    ruvia::task<void> receive_source(batch_source& source, ruvia::stop_token stop) {
        while (!stop.stop_requested()) {
            if (!source.waiting_) {
                auto received = co_await source.receiver_.receive(stop);
                if (!received.has_value()) {
                    if (!stop.stop_requested()) {
                        source.closed_ = true;
                    }
                    (void)ready_sender_.send(true);
                    co_return;
                }
                source.waiting_ = std::move(received).take_value();
                if (!source.waiting_) {
                    throw std::invalid_argument("empty node log batch");
                }
                source.received_events_ += static_cast<std::uint64_t>(source.waiting_->value().events_size());
            }
            const auto events = static_cast<std::size_t>(source.waiting_->value().events_size());
            std::size_t bytes{};
            for (const auto& event : source.waiting_->value().events()) {
                bytes += event.ByteSizeLong();
            }
            // The received value remains owned by this control coroutine while
            // capacity is exhausted. It is never handed back to a producer.
            while (source.retained_events_ + events > source.maximum_events_ ||
                   source.retained_bytes_ + bytes > source.maximum_bytes_) {
                if (!(co_await source.space_receiver_->receive(stop)).has_value()) {
                    // Retain this already accepted immutable value across cancellation.
                    co_return;
                }
            }
            if (stop.stop_requested()) {
                co_return;
            }
            source.pending_.push_back({.value_ = std::move(source.waiting_)});
            source.retained_events_ += events;
            source.retained_bytes_ += bytes;
            (void)ready_sender_.send(true);
        }
    }

    void prepare_delivery(batch_source& source, const std::string& node_id) {
        in_flight_delivery delivery{.source_ = &source};
        delivery.value_.set_node_id(node_id);
        auto wire_bytes = delivery.value_.ByteSizeLong();
        while (!source.pending_.empty() && delivery.event_count_ < log_contract::max_delivery_events) {
            auto& batch = source.pending_.front();
            const auto& event = batch.value_->value().events(batch.next_event_);
            const auto bytes = event.ByteSizeLong();
            const auto wire = log_batch::event_wire_bytes(bytes);
            if (wire_bytes + wire > log_contract::max_delivery_bytes) {
                break;
            }
            *delivery.value_.add_events() = event;
            ++delivery.event_count_;
            delivery.event_bytes_ += bytes;
            wire_bytes += wire;
            if (++batch.next_event_ == batch.value_->value().events_size()) {
                source.pending_.pop_front();
            }
        }
        in_flight_ = std::move(delivery);
    }

    batch_source data_;
    batch_source control_;
    ruvia::channel_sender<bool> ready_sender_;
    std::optional<ruvia::channel_receiver<bool>> ready_receiver_;
    std::optional<in_flight_delivery> in_flight_;
    channel_log channel_log_;
};

}  // namespace flexedge::node
