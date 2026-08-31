#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <ruvia/core/Task.h>
#include <ruvia/core/Timer.h>
#include <ruvia/core/StopToken.h>
#include <ruvia/web/WebSocketClient.h>

#include "node/data/data_plane.h"
#include "node/control/control_stream.h"
#include "node/proto/artifact.h"
#include "node/proto/control_protocol.h"
#include "node/proto/edge_control.pb.h"
#include "node/runtime/log_buffer.h"
#include "node/runtime/node_credentials.h"
#include "node/runtime/runtime_state.h"
#include "node/runtime/secret_buffer.h"
#include "node/runtime/state_store.h"
#include "node/runtime/version.h"

namespace flexedge::node {

struct ControlChannelConfig final {
    ruvia::WebSocketClientConfig webSocket;
    ruvia::StopToken stopToken;
    std::string agentVersion{std::string(kNodeVersion)};
    std::function<void(std::string_view)> nodeReleaseAvailable;
};

class ControlChannel final {
  public:
    ControlChannel(ruvia::EventLoop loop, ControlChannelConfig config, StateStore store,
                   NodeCredentials& credentials, RuntimeState& runtime, DataPlane& dataPlane,
                   NodeLogBuffer& logs)
        : loop_(std::move(loop)), config_(std::move(config)), store_(std::move(store)),
          credentials_(credentials), runtime_(runtime), dataPlane_(dataPlane), logs_(logs) {}

    ruvia::Task<void> run() {
        const auto worker = loop_.handle();
        auto retryDelay = std::chrono::seconds(1);
        while (!config_.stopToken.stopRequested()) {
            std::string errorText;
            try {
                ruvia::WebSocketClient client(loop_, config_.webSocket);
                co_await runSession(client);
                retryDelay = std::chrono::seconds(1);
            } catch (const ControlStreamEnded&) {
                retryDelay = std::chrono::seconds(1);
            } catch (const ruvia::WebSocketClientError& error) {
                errorText = error.what();
            } catch (const std::exception& error) {
                errorText = error.what();
            }
            if (config_.stopToken.stopRequested()) {
                co_return;
            }
            if (!errorText.empty()) {
                lastError_ = errorText;
                std::cerr << "flexedge node control channel failed: " << errorText << '\n';
                logs_.node("error", "control", errorText);
            }
            const auto slept = co_await ruvia::sleepFor(worker, retryDelay, config_.stopToken);
            if (slept == ruvia::TimerSleepResult::kStopRequested) {
                co_return;
            }
            retryDelay = (std::min)(retryDelay * 2, std::chrono::seconds(30));
        }
    }

  private:
    static std::string serialize(const v2::ClientEnvelope& envelope) {
        return serializeArtifact(envelope);
    }

    static bool differs(std::int64_t nodeSpecRevision, std::string_view releaseId,
                        std::string_view manifestDigest, std::int64_t desiredNodeSpecRevision,
                        std::string_view desiredReleaseId, std::string_view desiredManifestDigest) {
        return nodeSpecRevision != desiredNodeSpecRevision || releaseId != desiredReleaseId ||
               manifestDigest != desiredManifestDigest;
    }

    void notifyNodeRelease(std::string_view digest) const {
        if (config_.nodeReleaseAvailable) {
            config_.nodeReleaseAvailable(digest);
        }
    }

    v2::ClientEnvelope authenticateEnvelope(std::string requestId) const {
        v2::ClientEnvelope envelope;
        envelope.set_request_id(std::move(requestId));
        auto* value = envelope.mutable_authenticate();
        value->set_node_id(credentials_.nodeId());
        value->set_secret(credentials_.secret());
        value->set_applied_node_spec_revision(runtime_.appliedNodeSpecRevision());
        value->set_active_release_id(runtime_.activeReleaseId());
        value->set_active_manifest_digest(runtime_.activeManifestDigest());
        value->set_agent_version(config_.agentVersion);
        value->set_session_purpose(v2::AGENT_SESSION_PURPOSE_CONTROL);
        return envelope;
    }

    ruvia::Task<v2::Welcome> authenticate(const ruvia::WebSocketClientHandle& client) {
        constexpr std::string_view requestId{"authenticate"};
        auto envelope = authenticateEnvelope(std::string(requestId));
        auto* secret = envelope.mutable_authenticate()->mutable_secret();
        SecretStringGuard secretCleanser(*secret);
        auto bytes = serialize(envelope);
        SecretStringGuard bytesCleanser(bytes);
        co_await client.binary(bytes);
        const auto welcomeEnvelope = co_await readServerEnvelope(client);
        if (welcomeEnvelope.request_id() != requestId || !welcomeEnvelope.has_welcome()) {
            throw std::runtime_error("control plane did not authenticate the device");
        }
        co_return welcomeEnvelope.welcome();
    }

    v2::ClientEnvelope heartbeat(std::string_view nodeId, std::uint64_t sequence) {
        v2::ClientEnvelope envelope;
        envelope.set_request_id("heartbeat-" + std::to_string(sequence));
        auto* value = envelope.mutable_heartbeat();
        value->set_node_id(nodeId);
        value->set_applied_node_spec_revision(runtime_.appliedNodeSpecRevision());
        value->set_active_release_id(runtime_.activeReleaseId());
        value->set_active_manifest_digest(runtime_.activeManifestDigest());
        value->set_agent_version(config_.agentVersion);
        const auto metrics = dataPlane_.metrics();
        value->set_cpu_usage(metrics.cpuUsage);
        value->set_memory_usage(metrics.memoryUsage);
        value->set_traffic_out_bps(metrics.trafficOutBps);
        value->set_connection_count(metrics.connectionCount);
        value->set_load_1m(metrics.load1m);
        value->set_health(lastError_.empty() && dataPlane_.healthy() ? "healthy" : "degraded");
        value->set_last_error(lastError_);
        value->set_queued_log_events(logs_.queuedEvents());
        value->set_dropped_log_events(logs_.droppedEvents());
        for (const auto& report : dataPlane_.originHealthReports()) {
            auto* item = value->add_origin_health();
            item->set_website_id(report.websiteId);
            item->set_origin_id(report.originId);
            item->set_status(report.status);
            item->set_checked_at_unix_millis(report.checkedAtUnixMillis);
            item->set_latency_millis(report.latencyMillis);
            item->set_last_error(report.lastError);
        }
        return envelope;
    }

    static v2::ClientEnvelope releaseProbe(std::uint64_t sequence) {
        v2::ClientEnvelope envelope;
        envelope.set_request_id("release-probe-" + std::to_string(sequence));
        envelope.mutable_release_probe();
        return envelope;
    }

    void applyPersisted(const v2::ActiveState& active,
                        const std::vector<v2::DeliveryObject>& objects) {
        auto state = std::make_shared<v2::ActiveState>(active);
        auto compiled = std::make_shared<const CompiledConfig>(state, objects);
        runtime_.validateNext(*compiled);
        auto reload = dataPlane_.prepare(compiled);
        dataPlane_.prime(reload);
        dataPlane_.activate(std::move(reload));
    }

    void apply(const v2::DesiredState& desired, const std::vector<v2::DeliveryObject>& objects,
               v2::ApplyPhase& phase) {
        v2::ActiveState active;
        *active.mutable_node_spec() = desired.node_spec();
        *active.mutable_release() = desired.release();
        phase = v2::APPLY_PHASE_STAGE;
        store_.stage(active, objects);
        phase = v2::APPLY_PHASE_VALIDATE;
        auto state = std::make_shared<v2::ActiveState>(active);
        auto compiled = std::make_shared<const CompiledConfig>(state, objects);
        runtime_.validateNext(*compiled);
        auto reload = dataPlane_.prepare(compiled);
        phase = v2::APPLY_PHASE_ACTIVATE;
        dataPlane_.prime(reload);
        try {
            store_.activateStaged();
        } catch (...) {
            dataPlane_.abort(reload);
            throw;
        }
        dataPlane_.activate(std::move(reload));
        lastError_.clear();
    }

    ruvia::Task<void> receiveDesiredState(const ruvia::WebSocketClientHandle& client,
                                          std::string_view nodeId,
                                          std::string_view expectedRequestId) {
        const auto envelope = co_await readServerEnvelope(client);
        if (envelope.request_id() != expectedRequestId || !envelope.has_desired_state()) {
            throw std::runtime_error("control plane did not send desired state");
        }
        const auto& desired = envelope.desired_state();
        if (!desired.has_node_spec() || !desired.has_release() ||
            desired.node_spec().content().node_id() != nodeId) {
            throw std::runtime_error("control plane sent invalid desired state");
        }

        std::unordered_map<std::string, v2::DeliveryObject> objectsByDigest;
        std::vector<std::string> missing;
        objectsByDigest.reserve(
            static_cast<std::size_t>(desired.release().content().objects_size()));
        for (const auto& reference : desired.release().content().objects()) {
            if (auto object = store_.loadObject(reference.digest_sha256()); object) {
                objectsByDigest.emplace(reference.digest_sha256(), std::move(*object));
            } else {
                missing.push_back(reference.digest_sha256());
            }
        }
        for (std::size_t offset = 0; offset < missing.size(); offset += 64) {
            v2::ClientEnvelope request;
            request.set_request_id(envelope.request_id() + "-objects-" +
                                   std::to_string(offset / 64));
            auto* payload = request.mutable_object_request();
            payload->set_node_id(nodeId);
            payload->set_release_id(desired.release().content().release_id());
            const auto end = (std::min)(missing.size(), offset + 64);
            for (auto index = offset; index < end; ++index) {
                payload->add_digest_sha256(missing[index]);
            }
            co_await client.binary(serialize(request));
            const auto response = co_await readServerEnvelope(client);
            if (response.request_id() != request.request_id() || !response.has_object_batch() ||
                response.object_batch().release_id() != desired.release().content().release_id()) {
                throw std::runtime_error("control plane did not return requested objects");
            }
            for (const auto& object : response.object_batch().objects()) {
                if (!objectsByDigest.emplace(object.digest_sha256(), object).second) {
                    throw std::runtime_error("control plane returned a duplicate release object");
                }
            }
        }
        std::vector<v2::DeliveryObject> objects;
        objects.reserve(static_cast<std::size_t>(desired.release().content().objects_size()));
        for (const auto& reference : desired.release().content().objects()) {
            const auto found = objectsByDigest.find(reference.digest_sha256());
            if (found == objectsByDigest.end()) {
                throw std::runtime_error("control plane omitted a requested release object");
            }
            objects.push_back(std::move(found->second));
        }

        v2::ClientEnvelope acknowledgement;
        acknowledgement.set_request_id(envelope.request_id());
        auto* result = acknowledgement.mutable_apply_result();
        result->set_node_id(nodeId);
        result->set_node_spec_revision(desired.node_spec().content().revision());
        result->set_release_id(desired.release().content().release_id());
        result->set_manifest_digest(desired.release().digest_sha256());
        v2::ApplyPhase phase = v2::APPLY_PHASE_STAGE;
        std::exception_ptr failure;
        try {
            apply(desired, objects, phase);
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
        if (failure) {
            std::rethrow_exception(failure);
        }
    }

    static void validateWelcome(const v2::Welcome& welcome) {
        if (welcome.node_id().empty() || welcome.desired_node_spec_revision() <= 0 ||
            welcome.desired_release_id().empty() ||
            !isSha256Digest(welcome.desired_manifest_digest()) ||
            !isSha256Digest(welcome.node_binary_sha256())) {
            throw std::runtime_error("control plane sent invalid welcome state");
        }
    }

    ruvia::Task<void> processReleaseProbe(const ruvia::WebSocketClientHandle& connection,
                                          std::string_view nodeId, std::uint64_t& sequence) {
        const auto request = releaseProbe(++sequence);
        co_await connection.binary(serialize(request));
        const auto acknowledgement = co_await readServerEnvelope(connection);
        if (acknowledgement.request_id() != request.request_id() ||
            !acknowledgement.has_release_probe_ack() ||
            !isSha256Digest(acknowledgement.release_probe_ack().node_binary_sha256()) ||
            acknowledgement.release_probe_ack().desired_node_spec_revision() < 1 ||
            acknowledgement.release_probe_ack().desired_release_id().empty() ||
            !isSha256Digest(acknowledgement.release_probe_ack().desired_manifest_digest())) {
            throw std::runtime_error("control plane did not acknowledge release probe");
        }
        const auto& value = acknowledgement.release_probe_ack();
        notifyNodeRelease(value.node_binary_sha256());
        if (differs(runtime_.appliedNodeSpecRevision(), runtime_.activeReleaseId(),
                    runtime_.activeManifestDigest(), value.desired_node_spec_revision(),
                    value.desired_release_id(), value.desired_manifest_digest())) {
            co_await receiveDesiredState(connection, nodeId, request.request_id());
        }
        co_return;
    }

    ruvia::Task<std::chrono::seconds>
    processHeartbeat(const ruvia::WebSocketClientHandle& connection, std::string_view nodeId,
                     std::uint64_t& sequence) {
        const auto request = heartbeat(nodeId, ++sequence);
        co_await connection.binary(serialize(request));
        const auto acknowledgement = co_await readServerEnvelope(connection);
        if (acknowledgement.request_id() != request.request_id() ||
            !acknowledgement.has_heartbeat_ack() ||
            !isSha256Digest(acknowledgement.heartbeat_ack().node_binary_sha256())) {
            throw std::runtime_error("control plane did not acknowledge heartbeat");
        }
        const auto& value = acknowledgement.heartbeat_ack();
        notifyNodeRelease(value.node_binary_sha256());
        const auto interval =
            std::chrono::seconds((std::max)(value.next_interval_seconds(), std::uint32_t{1}));
        co_return interval;
    }

    ruvia::Task<void> runControlLoop(const ruvia::WebSocketClientHandle& connection,
                                     const v2::Welcome& welcome) {
        const auto worker = loop_.handle();
        const auto& nodeId = welcome.node_id();
        auto interval = std::chrono::seconds(
            (std::max)(welcome.heartbeat_interval_seconds(), std::uint32_t{1}));
        std::uint64_t heartbeatSequence{};
        std::uint64_t releaseProbeSequence{};
        auto nextHeartbeat = std::chrono::steady_clock::now() + interval;
        auto nextReleaseProbe =
            std::chrono::steady_clock::now() + std::chrono::seconds(kReleaseProbeIntervalSeconds);
        for (;;) {
            if (config_.stopToken.stopRequested()) {
                co_return;
            }
            auto now = std::chrono::steady_clock::now();
            if (now >= nextReleaseProbe) {
                co_await processReleaseProbe(connection, nodeId, releaseProbeSequence);
                nextReleaseProbe = std::chrono::steady_clock::now() +
                                   std::chrono::seconds(kReleaseProbeIntervalSeconds);
            }

            // Release checks and configuration control must stay independent from log ingestion.
            now = std::chrono::steady_clock::now();
            if (now >= nextHeartbeat) {
                interval = co_await processHeartbeat(connection, nodeId, heartbeatSequence);
                nextHeartbeat = std::chrono::steady_clock::now() + interval;
            }
            now = std::chrono::steady_clock::now();
            const auto nextControlMessage = (std::min)(nextHeartbeat, nextReleaseProbe);
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(nextControlMessage - now);
            const auto delay = (std::min)(remaining, std::chrono::milliseconds(10));
            const auto slept = co_await ruvia::sleepFor(worker, delay, config_.stopToken);
            if (slept == ruvia::TimerSleepResult::kStopRequested) {
                co_return;
            }
        }
    }

    ruvia::Task<void> runSession(ruvia::WebSocketClient& client) {
        auto persistent = store_.load();
        if (persistent.active.has_node_spec()) {
            applyPersisted(persistent.active, persistent.objects);
        }
        if (config_.stopToken.stopRequested()) {
            co_return;
        }
        co_await client.connect();
        if (client.subprotocol() != kControlSubprotocol) {
            throw std::runtime_error("control plane did not select the required subprotocol");
        }
        if (config_.stopToken.stopRequested()) {
            co_return;
        }
        const auto connection = client.withOptions({.stopToken = config_.stopToken});
        const auto welcome = co_await authenticate(connection);
        validateWelcome(welcome);
        notifyNodeRelease(welcome.node_binary_sha256());
        const auto& nodeId = welcome.node_id();
        if (persistent.active.has_node_spec() &&
            persistent.active.node_spec().content().node_id() != nodeId) {
            throw std::runtime_error("persisted state belongs to another node");
        }
        if (differs(runtime_.appliedNodeSpecRevision(), runtime_.activeReleaseId(),
                    runtime_.activeManifestDigest(), welcome.desired_node_spec_revision(),
                    welcome.desired_release_id(), welcome.desired_manifest_digest())) {
            co_await receiveDesiredState(connection, nodeId, "authenticate");
        }
        lastError_.clear();
        co_await runControlLoop(connection, welcome);
    }

    ruvia::EventLoop loop_;
    ControlChannelConfig config_;
    StateStore store_;
    NodeCredentials& credentials_;
    RuntimeState& runtime_;
    DataPlane& dataPlane_;
    NodeLogBuffer& logs_;
    std::string lastError_;
};

} // namespace flexedge::node
