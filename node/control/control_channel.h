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
#include "node/control/connect.h"
#include "node/control/control_stream.h"
#include "node/data/data_plane.h"
#include "node/proto/artifact.h"
#include "node/proto/control_protocol.h"
#include "node/proto/control_reply.h"
#include "node/proto/edge_control.pb.h"
#include "node/runtime/config_activation.h"
#include "node/runtime/log_buffer.h"
#include "node/runtime/node_credentials.h"
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
        node_log_buffer& logs, self_updater& updater)
        : loop_(std::move(loop)),
          config_(std::move(config)),
          store_(std::move(store)),
          credentials_(credentials),
          runtime_(runtime),
          data_plane_(data_plane),
          logs_(logs),
          updater_(updater) {}

    ruvia::task<void> run() {
        const auto worker = loop_.handle();
        auto retry_delay = std::chrono::seconds(1);
        while (!config_.stop_token_.stop_requested()) {
            std::string error_text;
            try {
                ruvia::websocket_client client(loop_, config_.web_socket_);
                co_await run_session(client);
                retry_delay = std::chrono::seconds(1);
            } catch (const control_stream_ended&) {
                retry_delay = std::chrono::seconds(1);
            } catch (const ruvia::websocket_client_error& error) {
                error_text = error.what();
            } catch (const std::exception& error) {
                error_text = error.what();
            }
            if (config_.stop_token_.stop_requested()) {
                co_return;
            }
            if (!error_text.empty()) {
                last_error_ = error_text;
                std::cerr << "flexedge node control channel failed: " << error_text << '\n';
                logs_.node("error", "control", error_text);
            }
            const auto slept = co_await ruvia::sleep_for(worker, retry_delay, config_.stop_token_);
            if (slept == ruvia::timer_sleep_result::stop_requested) {
                co_return;
            }
            retry_delay = (std::min)(retry_delay * 2, std::chrono::seconds(30));
        }
    }

private:
    static std::string serialize(const v2::ClientEnvelope& envelope) {
        return serialize_artifact(envelope);
    }

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

    v2::ClientEnvelope heartbeat(std::string_view node_id, std::uint64_t sequence) {
        v2::ClientEnvelope envelope;
        envelope.set_request_id("heartbeat-" + std::to_string(sequence));
        auto* value = envelope.mutable_heartbeat();
        value->set_node_id(node_id);
        value->set_applied_node_spec_revision(runtime_.applied_node_spec_revision());
        value->set_active_release_id(runtime_.active_release_id());
        value->set_active_manifest_digest(runtime_.active_manifest_digest());
        value->set_agent_version(config_.agent_version_);
        const auto metrics = data_plane_.metrics();
        value->set_cpu_usage(metrics.cpu_usage_);
        value->set_memory_usage(metrics.memory_usage_);
        value->set_traffic_out_bps(metrics.traffic_out_bps_);
        value->set_connection_count(metrics.connection_count_);
        value->set_load_1m(metrics.load1m_);
        value->set_health(last_error_.empty() && data_plane_.healthy() ? "healthy" : "degraded");
        value->set_last_error(last_error_);
        value->set_queued_log_events(logs_.queued_events());
        value->set_dropped_log_events(logs_.dropped_events());
        for (const auto& report : data_plane_.origin_health_reports()) {
            auto* item = value->add_origin_health();
            item->set_website_id(report.website_id_);
            item->set_origin_id(report.origin_id_);
            item->set_status(report.status_);
            item->set_checked_at_unix_millis(report.checked_at_unix_millis_);
            item->set_latency_millis(report.latency_millis_);
            item->set_last_error(report.last_error_);
        }
        return envelope;
    }

    v2::ClientEnvelope release_probe(std::uint64_t sequence, std::uint32_t wait_seconds) {
        v2::ClientEnvelope envelope;
        envelope.set_request_id("release-probe-" + std::to_string(sequence));
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
        envelope.set_request_id("node-release-" + std::to_string(offset));
        auto* payload = envelope.mutable_node_release_request();
        payload->set_node_id(node_id);
        payload->set_digest_sha256(digest);
        payload->set_offset(offset);
        return envelope;
    }

    ruvia::task<void> update_node_release(const ruvia::websocket_client_handle& connection,
        std::string_view node_id, std::string_view digest) {
        if (!updater_.update_available(digest)) {
            co_return;
        }
        try {
            std::uint64_t offset{};
            std::uint64_t total_bytes{};
            std::string version;
            for (;;) {
                const auto request = node_release_request(node_id, digest, offset);
                co_await connection.binary(serialize(request));
                const auto response = co_await read_server_envelope(connection);
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

    ruvia::task<void> receive_desired_state(const ruvia::websocket_client_handle& client,
        std::string_view node_id,
        std::string_view expected_request_id) {
        const auto envelope = co_await read_server_envelope(client);
        if (envelope.request_id() != expected_request_id || !envelope.has_desired_state()) {
            throw std::runtime_error("control plane did not send desired state");
        }
        const auto& desired = envelope.desired_state();
        if (!desired.has_node_spec() || !desired.has_release() ||
            desired.node_spec().content().node_id() != node_id) {
            throw std::runtime_error("control plane sent invalid desired state");
        }

        std::unordered_map<std::string, v2::DeliveryObject> objects_by_digest;
        std::vector<std::string> missing;
        objects_by_digest.reserve(
            static_cast<std::size_t>(desired.release().content().objects_size()));
        for (const auto& reference : desired.release().content().objects()) {
            if (auto object = load_cached_object(reference.digest_sha256()); object) {
                objects_by_digest.emplace(reference.digest_sha256(), std::move(*object));
            } else {
                missing.push_back(reference.digest_sha256());
            }
        }
        for (std::size_t offset = 0; offset < missing.size(); offset += 64) {
            v2::ClientEnvelope request;
            request.set_request_id(envelope.request_id() + "-objects-" +
                                   std::to_string(offset / 64));
            auto* payload = request.mutable_object_request();
            payload->set_node_id(node_id);
            payload->set_release_id(desired.release().content().release_id());
            const auto end = (std::min)(missing.size(), offset + 64);
            for (auto index = offset; index < end; ++index) {
                payload->add_digest_sha256(missing[index]);
            }
            co_await client.binary(serialize(request));
            const auto response = co_await read_server_envelope(client);
            if (response.request_id() != request.request_id() || !response.has_object_batch() ||
                response.object_batch().release_id() != desired.release().content().release_id()) {
                throw std::runtime_error("control plane did not return requested objects");
            }
            for (const auto& object : response.object_batch().objects()) {
                if (!objects_by_digest.emplace(object.digest_sha256(), object).second) {
                    throw std::runtime_error("control plane returned a duplicate release object");
                }
            }
        }
        std::vector<v2::DeliveryObject> objects;
        objects.reserve(static_cast<std::size_t>(desired.release().content().objects_size()));
        for (const auto& reference : desired.release().content().objects()) {
            const auto found = objects_by_digest.find(reference.digest_sha256());
            if (found == objects_by_digest.end()) {
                throw std::runtime_error("control plane omitted a requested release object");
            }
            objects.push_back(std::move(found->second));
        }

        v2::ClientEnvelope acknowledgement;
        acknowledgement.set_request_id(envelope.request_id());
        auto* result = acknowledgement.mutable_apply_result();
        result->set_node_id(node_id);
        result->set_node_spec_revision(desired.node_spec().content().revision());
        result->set_release_id(desired.release().content().release_id());
        result->set_manifest_digest(desired.release().digest_sha256());
        v2::ApplyPhase phase = v2::APPLY_PHASE_STAGE;
        std::exception_ptr failure;
        try {
            activate_desired_config(store_, data_plane_, desired, objects, phase);
            last_error_.clear();
            result->set_applied(true);
            result->set_failed_phase(v2::APPLY_PHASE_UNSPECIFIED);
        } catch (const std::exception& error) {
            result->set_applied(false);
            result->set_failed_phase(phase);
            result->set_error_code(phase == v2::APPLY_PHASE_VALIDATE ? "artifact_invalid"
                                                                     : "apply_failed");
            result->set_error(error.what());
            result->set_retryable(phase != v2::APPLY_PHASE_VALIDATE);
            failure = std::current_exception();
        }
        co_await client.binary(serialize(acknowledgement));
        const auto committed = co_await read_server_envelope(client);
        if (!matches_apply_result_ack(committed, acknowledgement.request_id(), *result)) {
            throw std::runtime_error("control plane did not confirm committed apply result");
        }
        if (failure) {
            std::rethrow_exception(failure);
        }
    }

    ruvia::task<void> process_release_probe(const ruvia::websocket_client_handle& connection,
        std::string_view node_id, std::uint64_t& sequence,
        std::uint32_t wait_seconds) {
        const auto request = release_probe(++sequence, wait_seconds);
        co_await connection.binary(serialize(request));
        const auto acknowledgement = co_await read_server_envelope(connection);
        if (!matches_release_probe_ack(acknowledgement, request.request_id())) {
            throw std::runtime_error("control plane did not acknowledge release probe");
        }
        const auto& value = acknowledgement.release_probe_ack();
        if (differs(runtime_.applied_node_spec_revision(), runtime_.active_release_id(),
                runtime_.active_manifest_digest(), value.desired_node_spec_revision(),
                value.desired_release_id(), value.desired_manifest_digest())) {
            co_await receive_desired_state(connection, node_id, request.request_id());
        }
        co_await update_node_release(connection, node_id, value.node_binary_sha256());
        co_return;
    }

    ruvia::task<std::chrono::seconds>
    process_heartbeat(const ruvia::websocket_client_handle& connection, std::string_view node_id,
        std::uint64_t& sequence) {
        const auto request = heartbeat(node_id, ++sequence);
        co_await connection.binary(serialize(request));
        const auto acknowledgement = co_await read_server_envelope(connection);
        if (!matches_heartbeat_ack(acknowledgement, request.request_id())) {
            throw std::runtime_error("control plane did not acknowledge heartbeat");
        }
        const auto& value = acknowledgement.heartbeat_ack();
        co_await update_node_release(connection, node_id, value.node_binary_sha256());
        const auto interval =
            std::chrono::seconds((std::max)(value.next_interval_seconds(), std::uint32_t{1}));
        co_return interval;
    }

    ruvia::task<void> run_control_loop(const ruvia::websocket_client_handle& connection,
        const v2::Welcome& welcome) {
        const auto& node_id = welcome.node_id();
        auto interval = std::chrono::seconds(
            (std::max)(welcome.heartbeat_interval_seconds(), std::uint32_t{1}));
        std::uint64_t heartbeat_sequence{};
        std::uint64_t release_probe_sequence{};
        auto next_heartbeat = std::chrono::steady_clock::now();
        for (;;) {
            if (config_.stop_token_.stop_requested()) {
                co_return;
            }
            if (std::chrono::steady_clock::now() >= next_heartbeat) {
                interval = co_await process_heartbeat(connection, node_id, heartbeat_sequence);
                next_heartbeat = std::chrono::steady_clock::now() + interval;
            }
            const auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
                next_heartbeat - std::chrono::steady_clock::now())
                                       .count();
            if (remaining <= 0) {
                interval = co_await process_heartbeat(connection, node_id, heartbeat_sequence);
                next_heartbeat = std::chrono::steady_clock::now() + interval;
                continue;
            }
            // One outstanding watch owns the read path. A committed change wakes it;
            // the deadline leaves room for heartbeats without interleaving object replies.
            co_await process_release_probe(
                connection, node_id, release_probe_sequence,
                static_cast<std::uint32_t>(
                    (std::min<std::int64_t>)(remaining, release_watch_maximum_seconds)));
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
    std::optional<v2::DeliveryObject> load_cached_object(std::string_view digest) {
        try {
            return store_.load_object(digest);
        } catch (const stored_state_unusable& error) {
            store_.discard_object(digest);
            report_discarded_state(error.what());
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
        const auto& node_id = welcome.node_id();
        if (persistent.active_.has_node_spec() &&
            persistent.active_.node_spec().content().node_id() != node_id) {
            throw std::runtime_error("persisted state belongs to another node");
        }
        if (differs(runtime_.applied_node_spec_revision(), runtime_.active_release_id(),
                runtime_.active_manifest_digest(), welcome.desired_node_spec_revision(),
                welcome.desired_release_id(), welcome.desired_manifest_digest())) {
            co_await receive_desired_state(connection, node_id, "authenticate");
        }
        co_await update_node_release(connection, node_id, welcome.node_binary_sha256());
        last_error_.clear();
        co_await run_control_loop(connection, welcome);
    }

    ruvia::event_loop loop_;
    control_channel_config config_;
    state_store store_;
    node_credentials& credentials_;
    runtime_state& runtime_;
    data_plane& data_plane_;
    node_log_buffer& logs_;
    self_updater& updater_;
    std::string last_error_;
};

}  // namespace flexedge::node
