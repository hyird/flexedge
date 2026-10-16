#pragma once

#include <ruvia/core/channel.h>
#include <ruvia/core/timer.h>

#include <chrono>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "node/control/channel_log.h"
#include "node/control/control_stream.h"
#include "node/runtime/log_buffer.h"

namespace flexedge::node {

// Shares the authenticated control WebSocket. Only one bounded buffer batch
// is in flight; a missing acknowledgement retains that batch while control
// requests continue through their own mailboxes and the priority writer.
class log_delivery final {
public:
    log_delivery(node_log_buffer& logs, ruvia::channel_receiver<bool> queued)
        : logs_(logs),
          queued_(std::move(queued)),
          channel_log_(logs, "log") {}

    ruvia::task<void> run(control_stream& stream, std::string node_id) {
        const auto stop = stream.get_stop_token();
        std::optional<node_log_buffer::in_flight_delivery> delivery;
        struct restore_unacknowledged final {
            node_log_buffer& logs_;
            std::optional<node_log_buffer::in_flight_delivery>& delivery_;
            ~restore_unacknowledged() {
                if (delivery_) {
                    logs_.restore(std::move(*delivery_));
                }
            }
        } restore{logs_, delivery};
        while (!stop.stop_requested()) {
            if (!delivery) {
                if (!logs_.pending()) {
                    if (!(co_await queued_.receive(stop)).has_value()) {
                        co_return;
                    }
                    continue;
                }
                delivery = logs_.take(node_id);
                if (!delivery) {
                    continue;
                }
            }
            bool retry{};
            try {
                v2::ClientEnvelope envelope;
                *envelope.mutable_log_delivery() = delivery->value();
                auto request = stream.send(envelope, control_request_priority::logs);
                const auto acknowledgement = co_await request.receive(std::chrono::seconds(5));
                if (!acknowledgement.has_log_delivery_ack()) {
                    throw std::runtime_error("control plane did not acknowledge log delivery");
                }
                logs_.acknowledge(std::move(*delivery));
                delivery.reset();
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

private:
    node_log_buffer& logs_;
    ruvia::channel_receiver<bool> queued_;
    channel_log channel_log_;
};

}  // namespace flexedge::node
