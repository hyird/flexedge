#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include <asio/error.hpp>

#include <openssl/crypto.h>

#include <ruvia/core/Task.h>
#include <ruvia/web/Context.h>
#include <ruvia/web/Controller.h>
#include <ruvia/web/Model.h>
#include <ruvia/web/WebSocket.h>

#include "node/proto/artifact.h"
#include "node/proto/control_protocol.h"
#include "node/proto/control_reply.h"
#include "node/proto/edge_control.pb.h"
#include "service/common/http.h"
#include "service/domains/agent/agent_protocol.h"
#include "service/domains/agent/agent_command.service.h"
#include "service/domains/agent/agent_read.service.h"
#include "service/features/log_ingest/ingest.h"
#include "service/features/node_release/artifact.h"
#include "service/features/node_dispatch/notifications.h"
#include "service/utils/sensitive_string.h"

namespace service::agent {

class ControlProtocolMiddleware final : public ruvia::Middleware<ControlProtocolMiddleware> {
  public:
    ruvia::Task<void> handle(ruvia::Context& c, ruvia::Next& next) {
        if (c.req().header("Sec-WebSocket-Protocol").value_or("") !=
            flexedge::node::kControlSubprotocol) {
            auto response = c.error({.status = ruvia::http_status::kUpgradeRequired,
                                     .code = "control_protocol_required",
                                     .message = "unsupported node control protocol"});
            response.header("Upgrade", "websocket");
            response.header("Sec-WebSocket-Protocol", flexedge::node::kControlSubprotocol);
            c.respond(std::move(response));
            co_return;
        }
        co_await next();
    }
};

class AgentController final : public ruvia::Controller<AgentController> {
  public:
    RUVIA_CONTROLLER_GROUP("/api/agent")
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/install-node.sh", installNodeScript);
    RUVIA_GET("/node", downloadNode);
    const auto controlOptions = ruvia::WebSocketRouteConfig{
        .subprotocols = {std::string(flexedge::node::kControlSubprotocol)},
        .lifecycle =
            {
                .heartbeat =
                    {
                        .pingInterval = std::chrono::seconds(30),
                        .pongTimeout = std::chrono::seconds(15),
                    },
                .closeHandshakeTimeout = std::chrono::seconds(5),
            },
    };
    RUVIA_GET_WS_OPTIONS("/connect", connect, controlOptions, ControlProtocolMiddleware);
    RUVIA_ROUTES_END

  private:
    static constexpr std::uint32_t kHeartbeatIntervalSeconds{30};

    class WebSocketSessionEnded final {};

    struct AuthenticatedSession final {
        std::string requestId;
        AgentPrincipal principal;
        flexedge::node::v2::AgentSessionPurpose purpose;
        std::int64_t appliedNodeSpecRevision{};
        std::string activeReleaseId;
        std::string activeManifestDigest;
        std::uint32_t heartbeatInterval{};
        bool requireApplyAck{};
    };

    struct NodeReleaseTransfer final {
        service::node_release::Catalog::Snapshot release;
        std::string digest;
        std::uint64_t nextOffset{};
    };

    ruvia::Task<ruvia::HttpResponse> installNodeScript(ruvia::Context& c) {
        const auto release = service::node_release::current();
        const auto& installer = release->installer();
        c.header("cache-control", "no-store");
        c.header("content-disposition", "inline; filename=install-node.sh");
        c.header("x-content-type-options", "nosniff");
        co_return c.file(
            {.path = installer.path(), .contentType = "text/x-shellscript; charset=utf-8"});
    }

    ruvia::Task<ruvia::HttpResponse> downloadNode(ruvia::Context& c) {
        const auto snapshot = service::node_release::current();
        const auto& release = snapshot->binary();
        c.header("cache-control", "no-cache");
        c.header("content-disposition", "attachment; filename=flexedge-node");
        c.header("x-flexedge-node-version", snapshot->version());
        c.header("x-flexedge-node-sha256", release.digest());
        c.header("etag", release.entityTag());
        if (c.req().header("If-None-Match").value_or("") == release.entityTag()) {
            c.status(ruvia::http_status::kNotModified);
            co_return c.body(nullptr);
        }
        co_return c.file({.path = release.path(), .contentType = "application/octet-stream"});
    }

    static bool differs(std::int64_t nodeSpecRevision, std::string_view release,
                        std::string_view manifest, const DesiredSummary& desired) {
        return nodeSpecRevision != desired.nodeSpecRevision || release != desired.releaseId ||
               manifest != desired.manifestDigest;
    }

    static ruvia::Task<void> send(ruvia::Context& c,
                                  const flexedge::node::v2::ServerEnvelope& envelope) {
        std::string bytes;
        if (!envelope.SerializeToString(&bytes)) {
            service::common::throwAppError(ARTIFACT_INVALID);
        }
        try {
            co_await c.webSocket().binary(bytes);
        } catch (const std::system_error& error) {
            if (webSocketSessionEnded(error.code())) {
                throw WebSocketSessionEnded{};
            }
            throw;
        }
    }

    static ruvia::Task<std::optional<ruvia::WebSocketMessage>> read(ruvia::WebSocket& socket) {
        try {
            co_return co_await socket.read();
        } catch (const std::system_error& error) {
            if (webSocketSessionEnded(error.code())) {
                throw WebSocketSessionEnded{};
            }
            throw;
        }
    }

    static ruvia::Task<void> close(ruvia::WebSocket& socket, ruvia::WebSocketCloseOptions options) {
        try {
            co_await socket.close(options);
        } catch (const std::system_error& error) {
            if (webSocketSessionEnded(error.code())) {
                throw WebSocketSessionEnded{};
            }
            throw;
        }
    }

    ruvia::Task<void> pushDesiredState(ruvia::Context& c, std::string_view requestId,
                                       const AgentPrincipal& principal) {
        auto desired = co_await agentCommandService().desiredState(c, principal);
        flexedge::node::v2::ServerEnvelope response;
        response.set_request_id(requestId);
        *response.mutable_desired_state() = std::move(desired);
        co_await send(c, response);
    }

    ruvia::Task<void> serveLogIngest(ruvia::Context& c, const AgentPrincipal& principal) {
        auto& ws = c.webSocket();
        while (const auto message = co_await read(ws)) {
            flexedge::node::v2::ClientEnvelope incoming;
            if (!parseClientEnvelope(*message, incoming) ||
                !validRequestId(incoming.request_id()) || !incoming.has_log_delivery()) {
                co_await close(ws, {.code = 1003, .reason = "log delivery required"});
                co_return;
            }
            const auto& delivery = incoming.log_delivery();
            if (!service::log_ingest::validDelivery(delivery, principal.nodeId)) {
                co_await close(ws, {.code = 1008, .reason = "invalid log delivery"});
                co_return;
            }
            if (!co_await agentReadService().isCurrent(c, principal)) {
                co_await close(ws, {.code = 1008, .reason = "agent credentials expired"});
                co_return;
            }
            if (!co_await service::log_ingest::enqueue(c, principal.tenantId, principal.nodeId,
                                                       principal.agentId, delivery)) {
                co_await close(ws, {.code = 1011, .reason = "log queue unavailable"});
                co_return;
            }
            flexedge::node::v2::ServerEnvelope acknowledgement;
            acknowledgement.set_request_id(incoming.request_id());
            acknowledgement.mutable_log_delivery_ack();
            co_await send(c, acknowledgement);
        }
    }

    ruvia::Task<AuthenticatedSession>
    authenticateSession(ruvia::Context& c, flexedge::node::v2::ClientEnvelope& envelope) {
        const auto& authentication = envelope.authenticate();
        const auto purpose = authentication.session_purpose();
        const auto agentId = authentication.node_id();
        const auto appliedNodeSpecRevision = authentication.applied_node_spec_revision();
        const auto activeReleaseId = authentication.active_release_id();
        const auto activeManifestDigest = authentication.active_manifest_digest();
        service::utils::SensitiveString secret(std::string(authentication.secret()));
        auto* serializedSecret = envelope.mutable_authenticate()->mutable_secret();
        OPENSSL_cleanse(serializedSecret->data(), serializedSecret->size());
        serializedSecret->clear();
        auto principal = co_await agentCommandService().authenticate(c, agentId, secret.view());
        co_return AuthenticatedSession{
            .requestId = envelope.request_id(),
            .principal = std::move(principal),
            .purpose = purpose,
            .appliedNodeSpecRevision = appliedNodeSpecRevision,
            .activeReleaseId = activeReleaseId,
            .activeManifestDigest = activeManifestDigest,
            .heartbeatInterval = kHeartbeatIntervalSeconds,
            .requireApplyAck = authentication.require_apply_ack(),
        };
    }

    ruvia::Task<void> serveLogSession(ruvia::Context& c, const AuthenticatedSession& session) {
        flexedge::node::v2::ServerEnvelope welcome;
        welcome.set_request_id(session.requestId);
        welcome.mutable_welcome()->set_node_id(session.principal.nodeId);
        welcome.mutable_welcome()->set_node_binary_sha256(
            service::node_release::current()->binary().digest());
        co_await send(c, welcome);
        co_await serveLogIngest(c, session.principal);
    }

    ruvia::Task<bool> handleObjectRequest(ruvia::Context& c, const AuthenticatedSession& session,
                                          const flexedge::node::v2::ClientEnvelope& incoming) {
        const auto& request = incoming.object_request();
        if (!validObjectRequest(request) || request.node_id() != session.principal.nodeId) {
            co_await close(c.webSocket(), {.code = 1008, .reason = "invalid object request"});
            co_return false;
        }
        auto objects = co_await agentReadService().objects(
            c, session.principal, request.release_id(), request.digest_sha256());
        flexedge::node::v2::ServerEnvelope response;
        response.set_request_id(incoming.request_id());
        *response.mutable_object_batch() = std::move(objects);
        co_await send(c, response);
        co_return true;
    }

    ruvia::Task<bool> handleReleaseProbe(ruvia::Context& c, const AuthenticatedSession& session,
                                         const flexedge::node::v2::ClientEnvelope& incoming) {
        const auto& probe = incoming.release_probe();
        if (probe.wait_seconds() > flexedge::node::kReleaseWatchMaximumSeconds) {
            co_await close(c.webSocket(), {.code = 1008, .reason = "invalid watch deadline"});
            co_return false;
        }
        // Subscribe before reading: a commit between the query and wait remains queued.
        auto subscription = service::node_dispatch::notifications::hub().subscribe(
            c.worker(), session.principal.tenantId);
        auto desired = co_await agentReadService().desiredSummary(c, session.principal);
        const auto knownRevision = probe.applied_node_spec_revision() > 0
                                       ? probe.applied_node_spec_revision()
                                       : session.appliedNodeSpecRevision;
        const auto& knownRelease = probe.applied_node_spec_revision() > 0
                                       ? probe.active_release_id()
                                       : session.activeReleaseId;
        const auto& knownDigest = probe.applied_node_spec_revision() > 0
                                      ? probe.active_manifest_digest()
                                      : session.activeManifestDigest;
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(probe.wait_seconds());
        while (!differs(knownRevision, knownRelease, knownDigest, desired)) {
            const auto remaining = deadline - std::chrono::steady_clock::now();
            if (remaining <= std::chrono::steady_clock::duration::zero())
                break;
            const auto signal = co_await subscription.receiveFor(remaining, c.stopToken());
            if (!signal.hasValue() && signal.status() != ruvia::WorkerWaitStatus::kTimedOut)
                co_return false;
            desired = co_await agentReadService().desiredSummary(c, session.principal);
        }
        std::optional<flexedge::node::v2::DesiredState> state;
        if (differs(knownRevision, knownRelease, knownDigest, desired)) {
            state = co_await agentCommandService().desiredState(c, session.principal);
        }
        const auto replies = flexedge::node::releaseProbeReplies(
            incoming.request_id(), service::node_release::current()->binary().digest(),
            desired.nodeSpecRevision, desired.releaseId, desired.manifestDigest, std::move(state));
        for (const auto& reply : replies)
            co_await send(c, reply);
        co_return true;
    }

    ruvia::Task<bool> handleNodeReleaseRequest(ruvia::Context& c,
                                               const AuthenticatedSession& session,
                                               std::optional<NodeReleaseTransfer>& transfer,
                                               const flexedge::node::v2::ClientEnvelope& incoming) {
        const auto& request = incoming.node_release_request();
        if (!validNodeReleaseRequest(request) || request.node_id() != session.principal.nodeId) {
            co_await close(c.webSocket(), {.code = 1008, .reason = "invalid node release request"});
            co_return false;
        }

        if (!transfer) {
            if (request.offset() != 0) {
                co_await close(c.webSocket(),
                               {.code = 1008, .reason = "node release offset invalid"});
                co_return false;
            }
            const auto release = service::node_release::current();
            const auto totalBytes = release->binary().size();
            if (release->binary().digest() != request.digest_sha256() || totalBytes == 0 ||
                totalBytes > flexedge::node::kMaximumNodeReleaseBytes) {
                co_await close(c.webSocket(), {.code = 1011, .reason = "node release unavailable"});
                co_return false;
            }
            transfer.emplace(
                NodeReleaseTransfer{.release = release, .digest = request.digest_sha256()});
        }

        const auto totalBytes = static_cast<std::uint64_t>(transfer->release->binary().size());
        if (transfer->digest != request.digest_sha256() ||
            request.offset() != transfer->nextOffset || totalBytes == 0 ||
            totalBytes > flexedge::node::kMaximumNodeReleaseBytes) {
            co_await close(c.webSocket(),
                           {.code = 1008, .reason = "node release transfer invalid"});
            co_return false;
        }
        const auto bytes = co_await c.runBlocking([release = transfer->release,
                                                   offset = transfer->nextOffset] {
            return release->binary().contentsRange(offset, flexedge::node::kNodeReleaseChunkBytes);
        });
        if (bytes.empty()) {
            co_await close(c.webSocket(), {.code = 1011, .reason = "node release read failed"});
            co_return false;
        }

        flexedge::node::v2::ServerEnvelope response;
        response.set_request_id(incoming.request_id());
        auto* chunk = response.mutable_node_release_chunk();
        chunk->set_version(transfer->release->version());
        chunk->set_digest_sha256(transfer->digest);
        chunk->set_total_bytes(totalBytes);
        chunk->set_offset(transfer->nextOffset);
        chunk->set_data(bytes);
        transfer->nextOffset += static_cast<std::uint64_t>(bytes.size());
        if (transfer->nextOffset == totalBytes) {
            transfer.reset();
        }
        co_await send(c, response);
        co_return true;
    }

    ruvia::Task<bool> handleApplyResult(ruvia::Context& c, AuthenticatedSession& session,
                                        const flexedge::node::v2::ClientEnvelope& incoming) {
        const auto& result = incoming.apply_result();
        if (!validApplyResult(result) || result.node_id() != session.principal.nodeId) {
            co_await close(c.webSocket(), {.code = 1008, .reason = "invalid apply result"});
            co_return false;
        }
        co_await agentCommandService().recordApplyResult(c, session.principal, result);
        if (result.applied()) {
            session.appliedNodeSpecRevision = result.node_spec_revision();
            session.activeReleaseId = result.release_id();
            session.activeManifestDigest = result.manifest_digest();
        }
        if (session.requireApplyAck) {
            flexedge::node::v2::ServerEnvelope reply;
            reply.set_request_id(incoming.request_id());
            auto* ack = reply.mutable_apply_result_ack();
            ack->set_node_id(result.node_id());
            ack->set_node_spec_revision(result.node_spec_revision());
            ack->set_release_id(result.release_id());
            ack->set_manifest_digest(result.manifest_digest());
            ack->set_applied(result.applied());
            co_await send(c, reply);
        }
        co_return true;
    }

    ruvia::Task<bool> handleHeartbeat(ruvia::Context& c, const AuthenticatedSession& session,
                                      const flexedge::node::v2::ClientEnvelope& incoming) {
        if (!incoming.has_heartbeat() || !validHeartbeat(incoming.heartbeat())) {
            co_await close(c.webSocket(), {.code = 1008, .reason = "invalid heartbeat"});
            co_return false;
        }
        const auto& value = incoming.heartbeat();
        if (value.node_id() != session.principal.nodeId) {
            co_await close(c.webSocket(), {.code = 1008, .reason = "node mismatch"});
            co_return false;
        }
        co_await agentCommandService().heartbeat(c, session.principal, toHeartbeatReport(value));
        flexedge::node::v2::ServerEnvelope acknowledgement;
        acknowledgement.set_request_id(incoming.request_id());
        auto* ack = acknowledgement.mutable_heartbeat_ack();
        ack->set_next_interval_seconds(session.heartbeatInterval);
        ack->set_node_binary_sha256(service::node_release::current()->binary().digest());
        co_await send(c, acknowledgement);
        co_return true;
    }

    ruvia::Task<void> serveControlMessages(ruvia::Context& c, AuthenticatedSession& session) {
        auto& ws = c.webSocket();
        std::optional<NodeReleaseTransfer> releaseTransfer;
        while (const auto message = co_await read(ws)) {
            flexedge::node::v2::ClientEnvelope incoming;
            if (!parseClientEnvelope(*message, incoming)) {
                co_await close(ws, {.code = 1003, .reason = "protobuf binary required"});
                co_return;
            }
            if (!validRequestId(incoming.request_id())) {
                co_await close(ws, {.code = 1008, .reason = "invalid request id"});
                co_return;
            }
            if (incoming.has_object_request()) {
                if (!co_await handleObjectRequest(c, session, incoming)) {
                    co_return;
                }
                continue;
            }
            if (incoming.has_release_probe()) {
                if (!co_await handleReleaseProbe(c, session, incoming)) {
                    co_return;
                }
                continue;
            }
            if (incoming.has_node_release_request()) {
                if (!co_await handleNodeReleaseRequest(c, session, releaseTransfer, incoming)) {
                    co_return;
                }
                continue;
            }
            if (incoming.has_apply_result()) {
                if (!co_await handleApplyResult(c, session, incoming)) {
                    co_return;
                }
                continue;
            }
            if (!co_await handleHeartbeat(c, session, incoming)) {
                co_return;
            }
        }
    }

    ruvia::Task<void> serveControlSession(ruvia::Context& c, AuthenticatedSession session) {
        const auto desired = co_await agentReadService().desiredSummary(c, session.principal);
        flexedge::node::v2::ServerEnvelope welcome;
        welcome.set_request_id(session.requestId);
        auto* payload = welcome.mutable_welcome();
        payload->set_node_id(session.principal.nodeId);
        payload->set_desired_node_spec_revision(desired.nodeSpecRevision);
        payload->set_desired_release_id(desired.releaseId);
        payload->set_desired_manifest_digest(desired.manifestDigest);
        payload->set_heartbeat_interval_seconds(session.heartbeatInterval);
        payload->set_node_binary_sha256(service::node_release::current()->binary().digest());
        co_await send(c, welcome);
        if (differs(session.appliedNodeSpecRevision, session.activeReleaseId,
                    session.activeManifestDigest, desired)) {
            co_await pushDesiredState(c, session.requestId, session.principal);
        }
        co_await serveControlMessages(c, session);
    }

    ruvia::Task<void> connect(ruvia::Context& c) {
        try {
            co_await serveConnection(c);
        } catch (const WebSocketSessionEnded&) {
        }
    }

    static bool webSocketSessionEnded(const std::error_code& error) noexcept {
        return error == asio::error::operation_aborted || error == asio::error::eof ||
               error == asio::error::connection_reset || error == asio::error::connection_aborted ||
               error == asio::error::broken_pipe;
    }

    ruvia::Task<void> serveConnection(ruvia::Context& c) {
        auto& ws = c.webSocket();
        const auto first = co_await read(ws);
        flexedge::node::v2::ClientEnvelope envelope;
        if (!first || !parseClientEnvelope(*first, envelope) ||
            !validAuthenticationEnvelope(envelope)) {
            co_await close(ws, {.code = 1008, .reason = "authentication required"});
            co_return;
        }

        const auto session = co_await authenticateSession(c, envelope);
        if (session.purpose == flexedge::node::v2::AGENT_SESSION_PURPOSE_LOG_INGEST) {
            co_await serveLogSession(c, session);
            co_return;
        }
        co_await serveControlSession(c, session);
    }
};

} // namespace service::agent
