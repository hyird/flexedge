#pragma once

#include <chrono>
#include <cctype>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
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
#include "node/proto/edge_control.pb.h"
#include "service/common/http.h"
#include "service/domains/agent/agent.service.h"
#include "service/features/log_ingest/ingest.h"
#include "service/features/node_release/artifact.h"
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
                        .pongTimeout = std::chrono::seconds(10),
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

    static bool releaseId(std::string_view value) {
        return service::common::parseUuid(value).has_value();
    }

    static bool validRequestId(std::string_view value) {
        return !value.empty() && value.size() <= 96 &&
               std::ranges::all_of(
                   value, [](unsigned char ch) { return std::isalnum(ch) != 0 || ch == '-'; });
    }

    static bool validAuthenticate(const flexedge::node::v2::Authenticate& value) {
        const bool validNodeId = value.node_id().size() == 32 &&
                                 std::ranges::all_of(value.node_id(), [](unsigned char ch) {
                                     return std::isdigit(ch) != 0 || (ch >= 'a' && ch <= 'f');
                                 });
        const bool validSecret = value.secret().size() >= 32 && value.secret().size() <= 128 &&
                                 std::ranges::all_of(value.secret(), [](unsigned char ch) {
                                     return ch >= 0x21 && ch <= 0x7e;
                                 });
        const bool hasActiveRelease = !value.active_release_id().empty();
        const bool validSessionPurpose =
            value.session_purpose() == flexedge::node::v2::AGENT_SESSION_PURPOSE_CONTROL ||
            value.session_purpose() == flexedge::node::v2::AGENT_SESSION_PURPOSE_LOG_INGEST;
        return validNodeId && validSecret && value.applied_node_spec_revision() >= 0 &&
               hasActiveRelease == !value.active_manifest_digest().empty() &&
               (!hasActiveRelease ||
                (releaseId(value.active_release_id()) &&
                 flexedge::node::isSha256Digest(value.active_manifest_digest()))) &&
               value.agent_version().size() <= 64 && validSessionPurpose;
    }

    static bool parseEnvelope(const ruvia::WebSocketMessage& message,
                              flexedge::node::v2::ClientEnvelope& envelope) {
        if (!message.binary() || message.payload().size() >
                                     static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
            return false;
        }
        return envelope.ParseFromArray(message.payload().data(),
                                       static_cast<int>(message.payload().size()));
    }

    static bool validAuthenticationEnvelope(const flexedge::node::v2::ClientEnvelope& envelope) {
        return validRequestId(envelope.request_id()) && envelope.has_authenticate() &&
               validAuthenticate(envelope.authenticate());
    }

    static bool validHeartbeat(const flexedge::node::v2::Heartbeat& value) {
        return service::common::parseUuid(value.node_id()) &&
               value.applied_node_spec_revision() >= 1 && releaseId(value.active_release_id()) &&
               flexedge::node::isSha256Digest(value.active_manifest_digest()) &&
               value.agent_version().size() <= 64 && value.cpu_usage() >= 0 &&
               value.cpu_usage() <= 1 && value.memory_usage() >= 0 && value.memory_usage() <= 1 &&
               value.traffic_out_bps() >= 0 && value.connection_count() >= 0 &&
               value.load_1m() >= 0 &&
               value.queued_log_events() <=
                   static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) &&
               value.dropped_log_events() <=
                   static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) &&
               value.health().size() <= 64 && value.last_error().size() <= 1000 &&
               value.origin_health_size() <= 10000 &&
               std::ranges::all_of(value.origin_health(), [](const auto& item) {
                   return service::common::parseUuid(item.website_id()) &&
                          service::common::parseUuid(item.origin_id()) &&
                          (item.status() == "healthy" || item.status() == "unhealthy" ||
                           item.status() == "unknown") &&
                          item.checked_at_unix_millis() >= 0 && item.latency_millis() <= 600000 &&
                          item.last_error().size() <= 1000;
               });
    }

    static bool validObjectRequest(const flexedge::node::v2::ObjectRequest& value) {
        if (!service::common::parseUuid(value.node_id()) || !releaseId(value.release_id()) ||
            value.digest_sha256().empty() || value.digest_sha256().size() > 64) {
            return false;
        }
        return std::ranges::all_of(value.digest_sha256(), [](const auto& digest) {
            return flexedge::node::isSha256Digest(digest);
        });
    }

    static bool validApplyPhase(flexedge::node::v2::ApplyPhase phase) {
        return phase == flexedge::node::v2::APPLY_PHASE_STAGE ||
               phase == flexedge::node::v2::APPLY_PHASE_VALIDATE ||
               phase == flexedge::node::v2::APPLY_PHASE_ACTIVATE;
    }

    static bool validApplyResult(const flexedge::node::v2::ApplyResult& value) {
        if (!service::common::parseUuid(value.node_id()) || value.node_spec_revision() < 1 ||
            !releaseId(value.release_id()) ||
            !flexedge::node::isSha256Digest(value.manifest_digest()) ||
            value.error_code().size() > 64 || value.error().size() > 1000) {
            return false;
        }
        return value.applied()
                   ? value.failed_phase() == flexedge::node::v2::APPLY_PHASE_UNSPECIFIED &&
                         value.error_code().empty() && value.error().empty()
                   : validApplyPhase(value.failed_phase()) && !value.error_code().empty() &&
                         !value.error().empty();
    }

    static bool differs(std::int64_t nodeSpecRevision, std::string_view release,
                        std::string_view manifest, const DesiredSummary& desired) {
        return nodeSpecRevision != desired.nodeSpecRevision || release != desired.releaseId ||
               manifest != desired.manifestDigest;
    }

    static HeartbeatReport report(const flexedge::node::v2::Heartbeat& value) {
        HeartbeatReport result{
            .nodeId = value.node_id(),
            .appliedNodeSpecRevision = value.applied_node_spec_revision(),
            .activeReleaseId = value.active_release_id(),
            .activeManifestDigest = value.active_manifest_digest(),
            .agentVersion = value.agent_version(),
            .cpuUsage = value.cpu_usage(),
            .memoryUsage = value.memory_usage(),
            .trafficOutBps = value.traffic_out_bps(),
            .connectionCount = value.connection_count(),
            .load1m = value.load_1m(),
            .queuedLogEvents = static_cast<std::int64_t>(value.queued_log_events()),
            .droppedLogEvents = static_cast<std::int64_t>(value.dropped_log_events()),
            .health = value.health(),
            .lastError = value.last_error(),
            .originHealth = {},
        };
        result.originHealth.reserve(value.origin_health_size());
        for (const auto& item : value.origin_health()) {
            result.originHealth.push_back({.websiteId = item.website_id(),
                                           .originId = item.origin_id(),
                                           .status = item.status(),
                                           .checkedAtUnixMillis = item.checked_at_unix_millis(),
                                           .latencyMillis = item.latency_millis(),
                                           .lastError = item.last_error()});
        }
        return result;
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
        auto desired = co_await agentService().desiredState(c, principal);
        flexedge::node::v2::ServerEnvelope response;
        response.set_request_id(requestId);
        *response.mutable_desired_state() = std::move(desired);
        co_await send(c, response);
    }

    ruvia::Task<void> serveLogIngest(ruvia::Context& c, const AgentPrincipal& principal) {
        auto& ws = c.webSocket();
        while (const auto message = co_await read(ws)) {
            flexedge::node::v2::ClientEnvelope incoming;
            if (!parseEnvelope(*message, incoming) || !validRequestId(incoming.request_id()) ||
                !incoming.has_log_delivery()) {
                co_await close(ws, {.code = 1003, .reason = "log delivery required"});
                co_return;
            }
            const auto& delivery = incoming.log_delivery();
            if (!service::log_ingest::validDelivery(delivery, principal.nodeId)) {
                co_await close(ws, {.code = 1008, .reason = "invalid log delivery"});
                co_return;
            }
            if (!co_await agentService().isCurrent(c, principal)) {
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
        auto principal = co_await agentService().authenticate(c, agentId, secret.view());
        co_return AuthenticatedSession{
            .requestId = envelope.request_id(),
            .principal = std::move(principal),
            .purpose = purpose,
            .appliedNodeSpecRevision = appliedNodeSpecRevision,
            .activeReleaseId = activeReleaseId,
            .activeManifestDigest = activeManifestDigest,
            .heartbeatInterval = kHeartbeatIntervalSeconds,
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
        auto objects = co_await agentService().objects(c, session.principal, request.release_id(),
                                                       request.digest_sha256());
        flexedge::node::v2::ServerEnvelope response;
        response.set_request_id(incoming.request_id());
        *response.mutable_object_batch() = std::move(objects);
        co_await send(c, response);
        co_return true;
    }

    ruvia::Task<bool> handleReleaseProbe(ruvia::Context& c, const AuthenticatedSession& session,
                                         const flexedge::node::v2::ClientEnvelope& incoming) {
        const auto desired = co_await agentService().desiredSummary(c, session.principal);
        flexedge::node::v2::ServerEnvelope acknowledgement;
        acknowledgement.set_request_id(incoming.request_id());
        auto* ack = acknowledgement.mutable_release_probe_ack();
        ack->set_node_binary_sha256(service::node_release::current()->binary().digest());
        ack->set_desired_node_spec_revision(desired.nodeSpecRevision);
        ack->set_desired_release_id(desired.releaseId);
        ack->set_desired_manifest_digest(desired.manifestDigest);
        co_await send(c, acknowledgement);
        co_return true;
    }

    ruvia::Task<bool> handleApplyResult(ruvia::Context& c, const AuthenticatedSession& session,
                                        const flexedge::node::v2::ClientEnvelope& incoming) {
        const auto& result = incoming.apply_result();
        if (!validApplyResult(result) || result.node_id() != session.principal.nodeId) {
            co_await close(c.webSocket(), {.code = 1008, .reason = "invalid apply result"});
            co_return false;
        }
        co_await agentService().recordApplyResult(c, session.principal, result);
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
        co_await agentService().heartbeat(c, session.principal, report(value));
        flexedge::node::v2::ServerEnvelope acknowledgement;
        acknowledgement.set_request_id(incoming.request_id());
        auto* ack = acknowledgement.mutable_heartbeat_ack();
        ack->set_next_interval_seconds(session.heartbeatInterval);
        ack->set_node_binary_sha256(service::node_release::current()->binary().digest());
        co_await send(c, acknowledgement);
        co_return true;
    }

    ruvia::Task<void> serveControlMessages(ruvia::Context& c, const AuthenticatedSession& session) {
        auto& ws = c.webSocket();
        while (const auto message = co_await read(ws)) {
            flexedge::node::v2::ClientEnvelope incoming;
            if (!parseEnvelope(*message, incoming)) {
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

    ruvia::Task<void> serveControlSession(ruvia::Context& c, const AuthenticatedSession& session) {
        const auto desired = co_await agentService().desiredSummary(c, session.principal);
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
        if (!first || !parseEnvelope(*first, envelope) || !validAuthenticationEnvelope(envelope)) {
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
