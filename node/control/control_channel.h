#pragma once

#include <ruvia/core/stop_token.h>
#include <ruvia/core/task.h>
#include <ruvia/core/timer.h>
#include <ruvia/web/websocket_client.h>

#include <algorithm>
#include <chrono>
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

#include "node/control/authenticate.h"
#include "node/control/channel_log.h"
#include "node/control/connect.h"
#include "node/control/control_stream.h"
#include "node/control/log_delivery.h"
#include "node/data/data_plane.h"
#include "node/proto/artifact.h"
#include "node/proto/control_protocol.h"
#include "node/proto/control_reply.h"
#include "node/proto/edge_control.pb.h"
#include "node/runtime/config_activation.h"
#include "node/runtime/log_buffer.h"
#include "node/runtime/node_credentials.h"
#include "node/runtime/runtime_metrics.h"
#include "node/runtime/runtime_state.h"
#include "node/runtime/self_updater.h"
#include "node/runtime/state_store.h"
#include "node/runtime/version.h"

namespace flexedge::node {

struct control_channel_config final {
    ruvia::websocket_client_config web_socket_;
    ruvia::stop_token stop_token_;
    std::string agent_version_{std::string(node_version)};
};

class control_channel final {
public:
    control_channel(ruvia::event_loop loop, control_channel_config config, state_store store,
        node_credentials& credentials, runtime_state& runtime, data_plane& data_plane,
        node_log_buffer& logs, self_updater& updater, ruvia::channel_receiver<bool> queued)
        : loop_(std::move(loop)),
          config_(std::move(config)),
          store_(std::move(store)),
          credentials_(credentials),
          runtime_(runtime),
          data_plane_(data_plane),
          logs_(logs),
          updater_(updater),
          channel_log_(logs, "control"),
          log_delivery_(logs, std::move(queued)) {}

    ruvia::task<void> run() {
        const auto worker = loop_.handle();
        while (!config_.stop_token_.stop_requested()) {
            try {
                ruvia::websocket_client client(loop_, config_.web_socket_);
                std::exception_ptr failure;
                try {
                    co_await run_session(client);
                } catch (...) {
                    failure = std::current_exception();
                }
                co_await client.shutdown();
                if (failure) {
                    std::rethrow_exception(failure);
                }
            } catch (const control_stream_ended&) {
                if (!config_.stop_token_.stop_requested()) {
                    last_error_ = "control plane closed the connection";
                    channel_log_.disconnected();
                }
            } catch (const ruvia::websocket_client_error& error) {
                if (!config_.stop_token_.stop_requested()) {
                    last_error_ = error.what();
                    channel_log_.failed(error);
                }
            } catch (const std::exception& error) {
                if (!config_.stop_token_.stop_requested()) {
                    last_error_ = error.what();
                    channel_log_.failed(error.what(), channel_failure_kind::permanent);
                }
            }
            if (config_.stop_token_.stop_requested()) {
                co_return;
            }
            const auto slept = co_await ruvia::sleep_for(worker, channel_log_.retry_delay(), config_.stop_token_);
            if (slept == ruvia::timer_sleep_result::stop_requested) {
                co_return;
            }
        }
    }

private:
    static bool differs(std::int64_t node_spec_revision, std::string_view release_id,
        std::string_view manifest_digest, std::int64_t desired_node_spec_revision,
        std::string_view desired_release_id, std::string_view desired_manifest_digest) {
        return node_spec_revision != desired_node_spec_revision || release_id != desired_release_id ||
               manifest_digest != desired_manifest_digest;
    }

    v2::ClientEnvelope authenticate_envelope(std::string request_id) const {
        v2::ClientEnvelope envelope;
        envelope.set_request_id(std::move(request_id));
        auto* value = envelope.mutable_authenticate();
        value->set_require_apply_ack(true);
        value->set_multiplex_logs(true);
        value->set_node_id(credentials_.node_id());
        value->set_secret(credentials_.secret());
        value->set_applied_node_spec_revision(runtime_.applied_node_spec_revision());
        value->set_active_release_id(runtime_.active_release_id());
        value->set_active_manifest_digest(runtime_.active_manifest_digest());
        value->set_agent_version(config_.agent_version_);
        value->set_session_purpose(v2::AGENT_SESSION_PURPOSE_CONTROL);
        return envelope;
    }

    ruvia::task<v2::Welcome> authenticate(const ruvia::websocket_client_handle& client) {
        co_return co_await authenticate_control_envelope(client, authenticate_envelope("authenticate"),
            "control plane did not authenticate the device");
    }

    ruvia::task<v2::ClientEnvelope> heartbeat(std::string_view node_id) {
        v2::ClientEnvelope envelope;
        auto* value = envelope.mutable_heartbeat();
        value->set_node_id(node_id);
        value->set_applied_node_spec_revision(runtime_.applied_node_spec_revision());
        value->set_active_release_id(runtime_.active_release_id());
        value->set_active_manifest_digest(runtime_.active_manifest_digest());
        value->set_agent_version(config_.agent_version_);
        // The data plane answers with immutable copies made on its own loops.
        const auto report = co_await data_plane_.report(config_.stop_token_);
        std::optional<std::uint64_t> traffic_out_bytes;
        if (report.load_) {
            traffic_out_bytes = report.load_->traffic_out_bytes_;
        }
        const auto metrics = metrics_sampler_.sample(traffic_out_bytes);
        value->set_cpu_usage(metrics.cpu_usage_);
        value->set_memory_usage(metrics.memory_usage_);
        value->set_traffic_out_bps(metrics.traffic_out_bps_);
        if (report.load_) {
            value->set_connection_count(report.load_->connection_count_);
            value->set_connected_ip_count(report.load_->connected_ip_count_);
        }
        value->set_load_1m(metrics.load1m_);
        value->set_health(
            last_error_.empty() && report.origin_health_->all_healthy() ? "healthy" : "degraded");
        value->set_last_error(last_error_);
        value->set_queued_log_events(logs_.queued_events());
        value->set_dropped_log_events(logs_.dropped_events());
        for (const auto& origin : report.origin_health_->reports()) {
            auto* item = value->add_origin_health();
            item->set_website_id(origin.website_id_);
            item->set_origin_id(origin.origin_id_);
            item->set_status(origin.status_);
            item->set_checked_at_unix_millis(origin.checked_at_unix_millis_);
            item->set_latency_millis(origin.latency_millis_);
            item->set_last_error(origin.last_error_);
        }
        co_return envelope;
    }

    v2::ClientEnvelope release_probe(std::uint32_t wait_seconds) {
        v2::ClientEnvelope envelope;
        auto* probe = envelope.mutable_release_probe();
        probe->set_wait_seconds(wait_seconds);
        probe->set_applied_node_spec_revision(runtime_.applied_node_spec_revision());
        probe->set_active_release_id(runtime_.active_release_id());
        probe->set_active_manifest_digest(runtime_.active_manifest_digest());
        return envelope;
    }

    static v2::ClientEnvelope node_release_request(std::string_view node_id, std::string_view digest,
        std::uint64_t offset) {
        v2::ClientEnvelope envelope;
        auto* payload = envelope.mutable_node_release_request();
        payload->set_node_id(node_id);
        payload->set_digest_sha256(digest);
        payload->set_offset(offset);
        return envelope;
    }

    ruvia::task<void> update_node_release(control_stream& connection,
        std::string_view node_id, std::string_view digest) {
        if (!updater_.update_available(digest)) {
            co_return;
        }
        try {
            std::uint64_t offset{};
            std::uint64_t total_bytes{};
            std::string version;
            for (;;) {
                auto request = node_release_request(node_id, digest, offset);
                auto replies = connection.send(request);
                const auto response = co_await replies.receive();
                if (response.request_id() != request.request_id() ||
                    !response.has_node_release_chunk()) {
                    throw std::runtime_error("control plane did not return a node release chunk");
                }
                const auto& chunk = response.node_release_chunk();
                if (!valid_node_release_version(chunk.version()) || chunk.digest_sha256() != digest ||
                    chunk.total_bytes() == 0 || chunk.total_bytes() > maximum_node_release_bytes ||
                    chunk.offset() != offset || chunk.offset() >= chunk.total_bytes() ||
                    chunk.data().empty() || chunk.data().size() > node_release_chunk_bytes ||
                    static_cast<std::uint64_t>(chunk.data().size()) >
                        chunk.total_bytes() - chunk.offset()) {
                    throw std::runtime_error("control plane sent an invalid node release chunk");
                }
                if (offset == 0) {
                    version = chunk.version();
                    total_bytes = chunk.total_bytes();
                    updater_.begin(version, digest, total_bytes);
                } else if (chunk.version() != version || chunk.total_bytes() != total_bytes) {
                    throw std::runtime_error("control plane changed the node release mid-transfer");
                }
                updater_.append(chunk.offset(), chunk.data());
                offset = updater_.written_bytes();
                if (offset == total_bytes) {
                    if (!updater_.complete()) {
                        throw std::runtime_error("node release transfer did not complete");
                    }
                    updater_.commit();
                    co_return;
                }
            }
        } catch (...) {
            updater_.abort();
            throw;
        }
    }

    ruvia::task<void> receive_desired_state(control_stream& client,
        std::string_view node_id,
        v2::ServerEnvelope envelope) {
        if (!envelope.has_desired_state()) {
            throw std::runtime_error("control plane did not send desired state");
        }
        const auto& desired = envelope.desired_state();
        const auto contents = open_state(desired);
        if (!contents || contents->node_spec_.node_id() != node_id) {
            throw std::runtime_error("control plane sent invalid desired state");
        }
        const auto& release = contents->release_;

        apply_trace trace;
        std::unordered_map<std::string, v2::DeliveryObject> objects_by_digest;
        std::vector<std::string> missing;
        std::size_t discarded_objects{};
        objects_by_digest.reserve(static_cast<std::size_t>(release.objects_size()));
        trace.run_step("load_objects", [&] {
            for (const auto& reference : release.objects()) {
                if (auto object = load_cached_object(reference.digest_sha256(), discarded_objects); object) {
                    objects_by_digest.emplace(reference.digest_sha256(), std::move(*object));
                } else {
                    missing.push_back(reference.digest_sha256());
                }
            }
        });
        const auto referenced = static_cast<std::size_t>(release.objects_size());
        trace.log(apply_log_level::info,
            "release has " + std::to_string(referenced) + " objects: " +
                std::to_string(referenced - missing.size()) + " from the local cache, " +
                std::to_string(missing.size()) + " requested from the control plane");
        if (discarded_objects != 0) {
            trace.log(apply_log_level::warning,
                std::to_string(discarded_objects) +
                    " cached objects failed verification and were requested again");
        }
        // Only a fetch that happens is a step; its lines must not claim one otherwise.
        std::optional<apply_trace::step_start> fetch;
        if (!missing.empty()) {
            fetch = trace.begin_step("fetch_objects");
        }
        for (std::size_t offset = 0; offset < missing.size(); offset += 64) {
            v2::ClientEnvelope request;
            auto* payload = request.mutable_object_request();
            payload->set_node_id(node_id);
            payload->set_release_id(release.release_id());
            const auto end = (std::min)(missing.size(), offset + 64);
            for (auto index = offset; index < end; ++index) {
                payload->add_digest_sha256(missing[index]);
            }
            auto replies = client.send(request);
            const auto response = co_await replies.receive();
            if (response.request_id() != request.request_id() || !response.has_object_batch() ||
                response.object_batch().release_id() != release.release_id()) {
                throw std::runtime_error("control plane did not return requested objects");
            }
            for (const auto& object : response.object_batch().objects()) {
                if (!objects_by_digest.emplace(object.digest_sha256(), object).second) {
                    throw std::runtime_error("control plane returned a duplicate release object");
                }
            }
        }
        if (fetch) {
            trace.end_step(*fetch, true);
        }
        std::vector<v2::DeliveryObject> objects;
        objects.reserve(static_cast<std::size_t>(release.objects_size()));
        for (const auto& reference : release.objects()) {
            const auto found = objects_by_digest.find(reference.digest_sha256());
            if (found == objects_by_digest.end()) {
                throw std::runtime_error("control plane omitted a requested release object");
            }
            objects.push_back(std::move(found->second));
        }

        v2::ClientEnvelope acknowledgement;
        auto* result = acknowledgement.mutable_apply_result();
        result->set_node_id(node_id);
        result->set_node_spec_revision(contents->node_spec_.revision());
        result->set_release_id(release.release_id());
        result->set_manifest_digest(desired.release().digest_sha256());
        const auto failure = apply_desired_config(store_, data_plane_, desired, objects, trace, *result);
        if (!failure) {
            last_error_.clear();
        }
        auto committed_reply = client.send(acknowledgement);
        const auto committed = co_await committed_reply.receive();
        if (!matches_apply_result_ack(committed, acknowledgement.request_id(), *result)) {
            throw std::runtime_error("control plane did not confirm committed apply result");
        }
        if (failure) {
            std::rethrow_exception(failure);
        }
    }

    ruvia::task<void> process_release_probe(control_stream& connection,
        std::string_view node_id,
        std::uint32_t wait_seconds) {
        auto request = release_probe(wait_seconds);
        auto replies = connection.send(request);
        const auto acknowledgement = co_await replies.receive();
        if (!matches_release_probe_ack(acknowledgement, request.request_id())) {
            throw std::runtime_error("control plane did not acknowledge release probe");
        }
        const auto& value = acknowledgement.release_probe_ack();
        if (differs(runtime_.applied_node_spec_revision(), runtime_.active_release_id(),
                runtime_.active_manifest_digest(), value.desired_node_spec_revision(),
                value.desired_release_id(), value.desired_manifest_digest())) {
            co_await receive_desired_state(connection, node_id, co_await replies.receive());
        }
        co_await update_node_release(connection, node_id, value.node_binary_sha256());
        co_return;
    }

    ruvia::task<std::chrono::seconds>
    process_heartbeat(control_stream& connection, std::string_view node_id) {
        auto request = co_await heartbeat(node_id);
        auto replies = connection.send(request);
        const auto acknowledgement = co_await replies.receive();
        if (!matches_heartbeat_ack(acknowledgement, request.request_id())) {
            throw std::runtime_error("control plane did not acknowledge heartbeat");
        }
        const auto& value = acknowledgement.heartbeat_ack();
        co_await update_node_release(connection, node_id, value.node_binary_sha256());
        const auto interval =
            std::chrono::seconds((std::max)(value.next_interval_seconds(), std::uint32_t{1}));
        co_return interval;
    }

    ruvia::task<void> run_control_loop(control_stream& connection,
        const v2::Welcome& welcome) {
        const auto& node_id = welcome.node_id();
        auto interval = std::chrono::seconds(
            (std::max)(welcome.heartbeat_interval_seconds(), std::uint32_t{1}));
        interval = co_await process_heartbeat(connection, node_id);
        if (!config_.stop_token_.stop_requested()) {
            channel_log_.recovered();
        }
        auto next_heartbeat = std::chrono::steady_clock::now() + interval;
        connection.spawn_owned(log_delivery_.run(connection, node_id));
        for (;;) {
            if (config_.stop_token_.stop_requested()) {
                co_return;
            }
            if (std::chrono::steady_clock::now() >= next_heartbeat) {
                interval = co_await process_heartbeat(connection, node_id);
                if (!config_.stop_token_.stop_requested()) {
                    channel_log_.recovered();
                }
                next_heartbeat = std::chrono::steady_clock::now() + interval;
            }
            const auto watch_seconds =
                release_watch_seconds(next_heartbeat - std::chrono::steady_clock::now());
            if (watch_seconds == 0) {
                continue;
            }
            // The watch owns only its request mailbox; log acknowledgements
            // continue through the same connection while this request waits.
            co_await process_release_probe(connection, node_id, watch_seconds);
        }
    }

    void report_discarded_state(std::string_view reason) {
        std::string message("discarded unusable local node state (");
        message.append(reason);
        message.append("); desired state will be fetched from the control plane");
        std::cerr << "flexedge node " << message << '\n';
        logs_.node("warning", "state", message);
    }

    // Serves the last applied state while the control plane is unreachable. Local state this
    // build cannot use is moved aside so the session still connects and fetches desired state.
    persistent_state restore_persisted_state() {
        try {
            auto persistent = store_.load();
            if (persistent.active_.has_node_spec()) {
                activate_persisted_config(data_plane_, persistent.active_, persistent.objects_);
            }
            return persistent;
        } catch (const stored_state_unusable& error) {
            store_.discard_active();
            report_discarded_state(error.what());
        }
        return {};
    }

    // A cached object that fails verification is requested again; objects from the control
    // plane are still verified before they are staged.
    std::optional<v2::DeliveryObject> load_cached_object(std::string_view digest,
        std::size_t& discarded) {
        try {
            return store_.load_object(digest);
        } catch (const stored_state_unusable& error) {
            store_.discard_object(digest);
            report_discarded_state(error.what());
            ++discarded;
        }
        return std::nullopt;
    }

    ruvia::task<void> run_session(ruvia::websocket_client& client) {
        auto persistent = restore_persisted_state();
        if (config_.stop_token_.stop_requested()) {
            co_return;
        }
        co_await connect_control_transport(client, config_.stop_token_);
        if (client.subprotocol() != control_subprotocol) {
            throw std::runtime_error("control plane did not select the required subprotocol");
        }
        if (config_.stop_token_.stop_requested()) {
            co_return;
        }
        const auto connection = client.with_options({.stop_token_ = config_.stop_token_});
        const auto welcome = co_await authenticate(connection);
        if (!valid_welcome(welcome)) {
            throw std::runtime_error("control plane sent invalid welcome state");
        }
        if (!welcome.multiplex_logs()) {
            throw std::runtime_error("control plane does not support multiplexed node sessions; upgrade the control plane before this node");
        }
        const auto& node_id = welcome.node_id();
        if (persistent.active_.has_node_spec() &&
            persistent.contents_.node_spec_.node_id() != node_id) {
            throw std::runtime_error("persisted state belongs to another node");
        }
        const auto worker = loop_.handle();
        control_stream stream(worker, connection, config_.stop_token_);
        std::exception_ptr failure;
        try {
            auto initial = stream.expect("authenticate");
            stream.start();
            if (differs(runtime_.applied_node_spec_revision(), runtime_.active_release_id(),
                    runtime_.active_manifest_digest(), welcome.desired_node_spec_revision(),
                    welcome.desired_release_id(), welcome.desired_manifest_digest())) {
                co_await receive_desired_state(stream, node_id, co_await initial.receive());
            }
            co_await update_node_release(stream, node_id, welcome.node_binary_sha256());
            last_error_.clear();
            co_await run_control_loop(stream, welcome);
        } catch (...) {
            failure = std::current_exception();
        }
        co_await stream.shutdown();
        if (failure) {
            std::rethrow_exception(failure);
        }
    }

    ruvia::event_loop loop_;
    control_channel_config config_;
    state_store store_;
    node_credentials& credentials_;
    runtime_state& runtime_;
    data_plane& data_plane_;
    node_log_buffer& logs_;
    self_updater& updater_;
    channel_log channel_log_;
    log_delivery log_delivery_;
    std::string last_error_;
    runtime_metrics_sampler metrics_sampler_;
};

}  // namespace flexedge::node
