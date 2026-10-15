#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/context.h>
#include <ruvia/web/controller.h>
#include <ruvia/web/websocket.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <asio/error.hpp>
#include <openssl/crypto.h>

#include "ruvia/web/model.h"

#include "node/proto/artifact.h"
#include "node/proto/control_protocol.h"
#include "node/proto/control_reply.h"
#include "node/proto/edge_control.pb.h"
#include "service/common/http.h"
#include "service/domains/agent/agent_command_service.h"
#include "service/domains/agent/agent_protocol.h"
#include "service/domains/agent/agent_read_service.h"
#include "service/domains/agent/agent_report_mapper.h"
#include "service/domains/agent/agent_transport.h"
#include "service/domains/node/node_connection.h"
#include "service/features/attempt_trace/recorder.h"
#include "service/features/log_ingest/ingest.h"
#include "service/features/node_dispatch/fanout.h"
#include "service/features/node_release/catalog.h"
#include "service/features/sync_runtime/attempt_record.h"
#include "service/utils/sensitive_string.h"

namespace service::agent {

class control_protocol_middleware final {
public:
    ruvia::task<void> handle(ruvia::context& c, ruvia::next& next) {
        if (c.req().header("Sec-WebSocket-Protocol").value_or("") !=
            flexedge::node::control_subprotocol) {
            auto response = c.error({.status_ = ruvia::http_status::upgrade_required,
                .code_ = "control_protocol_required",
                .message_ = "unsupported node control protocol"});
            response.header("Upgrade", "websocket");
            response.header("Sec-WebSocket-Protocol", flexedge::node::control_subprotocol);
            c.respond(std::move(response));
            co_return;
        }
        co_await next();
    }
};

class agent_controller final {
public:
    RUVIA_CONTROLLER_GROUP("/api/agent")
    RUVIA_ROUTES_BEGIN(agent_controller)
    RUVIA_GET("/install-node.sh", install_node_script);
    RUVIA_GET("/node", download_node);
    const auto control_options = ruvia::websocket_route_config{
        .subprotocols_ = {std::string(flexedge::node::control_subprotocol)},
        .lifecycle_ =
            {
                .heartbeat_ =
                    {
                        .ping_interval_ = std::chrono::seconds(30),
                        .pong_timeout_ = std::chrono::seconds(15),
                    },
                .close_handshake_timeout_ = std::chrono::seconds(5),
            },
    };
    RUVIA_GET_WS_OPTIONS("/connect", connect, control_options, control_protocol_middleware);
    RUVIA_ROUTES_END

private:
    static constexpr auto heartbeat_interval_seconds =
        static_cast<std::uint32_t>(service::node::agent_heartbeat_interval.count());

    class web_socket_session_ended final {};

    struct authenticated_session final {
        std::string request_id_;
        agent_principal principal_;
        flexedge::node::v2::AgentSessionPurpose purpose_;
        std::int64_t applied_node_spec_revision_{};
        std::string active_release_id_;
        std::string active_manifest_digest_;
        std::uint32_t heartbeat_interval_{};
        bool require_apply_ack_{};
        // Release of the last desired state sent on this connection and when it
        // was sent (monotonic), timing the node's next apply result for it.
        std::string dispatched_release_id_;
        std::chrono::steady_clock::time_point dispatched_at_{};
        // Trace of the node's apply of the dispatched release, owned by this
        // connection; it ends with the node's ApplyResult for that release.
        std::unique_ptr<service::attempt_trace::recorder> apply_trace_{};
    };

    struct node_release_transfer final {
        service::node_release::catalog::snapshot release_;
        std::string digest_;
        std::uint64_t next_offset_{};
    };

    ruvia::task<ruvia::http_response> install_node_script(ruvia::context& c) {
        const auto release = service::node_release::current();
        const auto& installer = release->installer();
        c.header("cache-control", "no-store");
        c.header("content-disposition", "inline; filename=install-node.sh");
        c.header("x-content-type-options", "nosniff");
        co_return c.file(
            {.path_ = installer.path(), .content_type_ = "text/x-shellscript; charset=utf-8"});
    }

    ruvia::task<ruvia::http_response> download_node(ruvia::context& c) {
        const auto snapshot = service::node_release::current();
        const auto& release = snapshot->binary();
        c.header("cache-control", "no-cache");
        c.header("content-disposition", "attachment; filename=flexedge-node");
        c.header("x-flexedge-node-version", snapshot->version());
        c.header("x-flexedge-node-sha256", release.digest());
        c.header("etag", release.entity_tag());
        if (c.req().header("If-None-Match").value_or("") == release.entity_tag()) {
            c.status(ruvia::http_status::not_modified);
            co_return c.body(nullptr);
        }
        co_return c.file({.path_ = release.path(), .content_type_ = "application/octet-stream"});
    }

    static bool differs(std::int64_t node_spec_revision, std::string_view release,
        std::string_view manifest, const desired_summary& desired) {
        return node_spec_revision != desired.node_spec_revision_ || release != desired.release_id_ ||
               manifest != desired.manifest_digest_;
    }

    static ruvia::task<void> send(ruvia::context& c,
        const flexedge::node::v2::ServerEnvelope& envelope) {
        std::string bytes;
        if (!envelope.SerializeToString(&bytes)) {
            service::common::throw_app_error(artifact_invalid);
        }
        try {
            co_await c.get_websocket().binary(bytes);
        } catch (const std::system_error& error) {
            if (get_web_socket_session_ended(error.code())) {
                throw web_socket_session_ended{};
            }
            throw;
        }
    }

    static ruvia::task<std::optional<ruvia::websocket_message>> read(ruvia::websocket& socket) {
        try {
            co_return co_await socket.read();
        } catch (const std::system_error& error) {
            if (get_web_socket_session_ended(error.code())) {
                throw web_socket_session_ended{};
            }
            throw;
        }
    }

    static ruvia::task<void> close(ruvia::websocket& socket, ruvia::websocket_close_options options) {
        try {
            co_await socket.close(options);
        } catch (const std::system_error& error) {
            if (get_web_socket_session_ended(error.code())) {
                throw web_socket_session_ended{};
            }
            throw;
        }
    }

    ruvia::task<void> push_desired_state(ruvia::context& c, std::string_view request_id,
        authenticated_session& session) {
        auto desired = co_await get_agent_command_service().desired_state(c, session.principal_);
        flexedge::node::v2::ServerEnvelope response;
        response.set_request_id(request_id);
        *response.mutable_desired_state() = std::move(desired);
        co_await send(c, response);
        record_dispatch(session, response);
    }

    // Times the node's next apply result for the dispatched release and starts
    // its trace. A trace that cannot be started leaves the dispatch untouched.
    static void record_dispatch(authenticated_session& session, const flexedge::node::v2::ServerEnvelope& envelope) {
        const auto& content = envelope.desired_state().release().content();
        session.dispatched_release_id_ = content.release_id();
        session.dispatched_at_ = std::chrono::steady_clock::now();
        try {
            session.apply_trace_ = std::make_unique<service::attempt_trace::recorder>(
                service::attempt_trace::trace_kind::node_apply, "web", session.dispatched_at_);
            auto& trace = *session.apply_trace_;
            auto step = trace.begin_step(service::attempt_trace::step_code::send_desired_state);
            const std::array fields{service::attempt_trace::node_message_field{.name_ = "release_id",
                .value_ = content.release_id()}};
            trace.record_node_message({.direction_ = "sent",
                                          .message_ = "DesiredState",
                                          .request_id_ = envelope.request_id(),
                                          .object_count_ = content.objects_size(),
                                          .bytes_ = static_cast<std::int64_t>(envelope.ByteSizeLong()),
                                          .fields_ = fields},
                session.dispatched_at_);
            step.complete();
        } catch (...) {
            session.apply_trace_.reset();
        }
    }

    // The trace of the apply `result` answers: the one started when this
    // connection dispatched that release, otherwise none.
    static std::unique_ptr<service::attempt_trace::recorder> take_apply_trace(authenticated_session& session,
        std::string_view release_id) {
        if (session.dispatched_release_id_.empty() || session.dispatched_release_id_ != release_id) {
            return {};
        }
        return std::move(session.apply_trace_);
    }

    // Start of a node's apply as its earliest reported step, on this
    // process's monotonic clock; now when the node reported none.
    static std::chrono::steady_clock::time_point reported_apply_start(const flexedge::node::v2::ApplyResult& result) {
        const auto now = std::chrono::steady_clock::now();
        const auto wall_now = std::chrono::system_clock::now();
        auto earliest = wall_now;
        for (const auto& step : result.steps()) {
            earliest = std::min(earliest, reported_time(step.started_unix_ms(), wall_now));
        }
        return now - std::chrono::duration_cast<std::chrono::steady_clock::duration>(wall_now - earliest);
    }

    // A node's wall-clock time, or now when it is missing or more than a day off.
    static std::chrono::system_clock::time_point reported_time(std::int64_t unix_ms,
        std::chrono::system_clock::time_point now) {
        const auto value = std::chrono::system_clock::time_point(std::chrono::milliseconds(unix_ms));
        if (unix_ms <= 0 || value > now + std::chrono::hours(24) || value < now - std::chrono::hours(24)) {
            return now;
        }
        return value;
    }

    // Adds the node's own apply steps and log lines (ApplyResult fields 10 and
    // 11, node 0.4.1+), within the node's caps, through the trace redactor.
    static void import_reported_apply(service::attempt_trace::recorder& trace,
        const flexedge::node::v2::ApplyResult& result) {
        constexpr int maximum_steps = 16;
        constexpr int maximum_logs = 100;
        const auto now = std::chrono::system_clock::now();
        for (int index = 0; index < std::min(result.steps_size(), maximum_steps); ++index) {
            const auto& step = result.steps(index);
            const auto code = service::attempt_trace::node_phase_step(step.phase());
            trace.record_reported_step(code, step.phase(),
                code == service::attempt_trace::step_code::node_step ? std::string_view(step.phase()) : std::string_view{},
                reported_time(step.started_unix_ms(), now),
                std::clamp<std::int64_t>(step.duration_ms(), 0, 24LL * 3600 * 1000), step.ok(), step.error());
        }
        for (int index = 0; index < std::min(result.logs_size(), maximum_logs); ++index) {
            const auto& line = result.logs(index);
            const auto level = line.level() == "error"     ? service::attempt_trace::log_level::error
                               : line.level() == "warning" ? service::attempt_trace::log_level::warning
                                                           : service::attempt_trace::log_level::info;
            trace.record_reported_log(level, reported_time(line.unix_ms(), now), line.phase(), line.message());
        }
    }

    static std::string_view reported_phase_name(flexedge::node::v2::ApplyPhase phase) noexcept {
        switch (phase) {
            case flexedge::node::v2::APPLY_PHASE_STAGE:
                return "stage";
            case flexedge::node::v2::APPLY_PHASE_VALIDATE:
                return "validate";
            case flexedge::node::v2::APPLY_PHASE_ACTIVATE:
                return "activate";
            default:
                return "";
        }
    }

    // Milliseconds since this connection sent the desired state `release_id`
    // answers; each dispatch times one result.
    static std::optional<std::int64_t> take_dispatch_elapsed(authenticated_session& session,
        std::string_view release_id) {
        if (session.dispatched_release_id_.empty() || session.dispatched_release_id_ != release_id) {
            return std::nullopt;
        }
        session.dispatched_release_id_.clear();
        const auto elapsed = service::sync_runtime::elapsed_milliseconds(session.dispatched_at_);
        session.dispatched_at_ = {};
        return elapsed;
    }

    ruvia::task<void> serve_log_ingest(ruvia::context& c, const agent_principal& principal) {
        auto& ws = c.get_websocket();
        while (const auto message = co_await read(ws)) {
            flexedge::node::v2::ClientEnvelope incoming;
            if (!parse_client_envelope(*message, incoming) ||
                !valid_request_id(incoming.request_id()) || !incoming.has_log_delivery()) {
                co_await close(ws, {.code_ = 1003, .reason_ = "log delivery required"});
                co_return;
            }
            const auto& delivery = incoming.log_delivery();
            if (!service::log_ingest::valid_delivery(delivery, principal.node_id_)) {
                co_await close(ws, {.code_ = 1008, .reason_ = "invalid log delivery"});
                co_return;
            }
            if (!co_await get_agent_read_service().is_current(c, principal)) {
                co_await close(ws, {.code_ = 1008, .reason_ = "agent credentials expired"});
                co_return;
            }
            if (!co_await service::log_ingest::enqueue(c, principal.tenant_id_, principal.node_id_,
                    principal.agent_id_, delivery)) {
                co_await close(ws, {.code_ = 1011, .reason_ = "log queue unavailable"});
                co_return;
            }
            flexedge::node::v2::ServerEnvelope acknowledgement;
            acknowledgement.set_request_id(incoming.request_id());
            acknowledgement.mutable_log_delivery_ack();
            co_await send(c, acknowledgement);
        }
    }

    ruvia::task<authenticated_session>
    authenticate_session(ruvia::context& c, flexedge::node::v2::ClientEnvelope& envelope) {
        const auto& authentication = envelope.authenticate();
        const auto purpose = authentication.session_purpose();
        const auto agent_id = authentication.node_id();
        const auto applied_node_spec_revision = authentication.applied_node_spec_revision();
        const auto active_release_id = authentication.active_release_id();
        const auto active_manifest_digest = authentication.active_manifest_digest();
        service::utils::sensitive_string secret(std::string(authentication.secret()));
        auto* serialized_secret = envelope.mutable_authenticate()->mutable_secret();
        OPENSSL_cleanse(serialized_secret->data(), serialized_secret->size());
        serialized_secret->clear();
        auto principal = co_await get_agent_command_service().authenticate(c, agent_id, secret.view());
        co_return authenticated_session{
            .request_id_ = envelope.request_id(),
            .principal_ = std::move(principal),
            .purpose_ = purpose,
            .applied_node_spec_revision_ = applied_node_spec_revision,
            .active_release_id_ = active_release_id,
            .active_manifest_digest_ = active_manifest_digest,
            .heartbeat_interval_ = heartbeat_interval_seconds,
            .require_apply_ack_ = authentication.require_apply_ack(),
            .dispatched_release_id_ = {},
            .dispatched_at_ = {},
        };
    }

    ruvia::task<void> serve_log_session(ruvia::context& c, const authenticated_session& session) {
        flexedge::node::v2::ServerEnvelope welcome;
        welcome.set_request_id(session.request_id_);
        welcome.mutable_welcome()->set_node_id(session.principal_.node_id_);
        welcome.mutable_welcome()->set_node_binary_sha256(
            service::node_release::current()->binary().digest());
        co_await send(c, welcome);
        co_await serve_log_ingest(c, session.principal_);
    }

    ruvia::task<bool> handle_object_request(ruvia::context& c, const authenticated_session& session,
        const flexedge::node::v2::ClientEnvelope& incoming) {
        const auto& request = incoming.object_request();
        if (!valid_object_request(request) || request.node_id() != session.principal_.node_id_) {
            co_await close(c.get_websocket(), {.code_ = 1008, .reason_ = "invalid object request"});
            co_return false;
        }
        const auto received_at = std::chrono::steady_clock::now();
        auto objects = co_await get_agent_read_service().objects(
            c, session.principal_, request.release_id(), request.digest_sha256());
        flexedge::node::v2::ServerEnvelope response;
        response.set_request_id(incoming.request_id());
        *response.mutable_object_batch() = std::move(objects);
        co_await send(c, response);
        if (session.apply_trace_ && session.dispatched_release_id_ == request.release_id()) {
            auto& trace = *session.apply_trace_;
            auto step = trace.begin_step(service::attempt_trace::step_code::serve_objects);
            trace.record_node_message({.direction_ = "received",
                                          .message_ = "ObjectRequest",
                                          .request_id_ = incoming.request_id(),
                                          .object_count_ = request.digest_sha256_size(),
                                          .bytes_ = static_cast<std::int64_t>(incoming.ByteSizeLong()),
                                          .fields_ = {}},
                received_at);
            trace.record_node_message({.direction_ = "sent",
                                          .message_ = "ObjectBatch",
                                          .request_id_ = response.request_id(),
                                          .object_count_ = response.object_batch().objects_size(),
                                          .bytes_ = static_cast<std::int64_t>(response.ByteSizeLong()),
                                          .fields_ = {}},
                std::chrono::steady_clock::now());
            step.complete();
        }
        co_return true;
    }

    ruvia::task<bool> handle_release_probe(ruvia::context& c, authenticated_session& session,
        const flexedge::node::v2::ClientEnvelope& incoming) {
        const auto& probe = incoming.release_probe();
        if (probe.wait_seconds() > flexedge::node::release_watch_maximum_seconds) {
            co_await close(c.get_websocket(), {.code_ = 1008, .reason_ = "invalid watch deadline"});
            co_return false;
        }
        // Subscribe before reading: a commit between the query and wait remains queued.
        auto subscription = service::node_dispatch::fanout::get_hub(c).subscribe(
            c.worker(), session.principal_.tenant_id_);
        auto desired = co_await get_agent_read_service().desired_summary(c, session.principal_);
        const auto known_revision = probe.applied_node_spec_revision() > 0
                                        ? probe.applied_node_spec_revision()
                                        : session.applied_node_spec_revision_;
        const auto& known_release = probe.applied_node_spec_revision() > 0
                                        ? probe.active_release_id()
                                        : session.active_release_id_;
        const auto& known_digest = probe.applied_node_spec_revision() > 0
                                       ? probe.active_manifest_digest()
                                       : session.active_manifest_digest_;
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(probe.wait_seconds());
        while (!differs(known_revision, known_release, known_digest, desired)) {
            const auto remaining = deadline - std::chrono::steady_clock::now();
            if (remaining <= std::chrono::steady_clock::duration::zero()) {
                break;
            }
            const auto signal = co_await subscription.receive_for(remaining, c.get_stop_token());
            if (!signal.has_value() && signal.status() != ruvia::worker_wait_status::timed_out) {
                co_return false;
            }
            desired = co_await get_agent_read_service().desired_summary(c, session.principal_);
        }
        std::optional<flexedge::node::v2::DesiredState> state;
        if (differs(known_revision, known_release, known_digest, desired)) {
            state = co_await get_agent_command_service().desired_state(c, session.principal_);
        }
        const auto replies = flexedge::node::release_probe_replies(
            incoming.request_id(), service::node_release::current()->binary().digest(),
            desired.node_spec_revision_, desired.release_id_, desired.manifest_digest_, std::move(state));
        for (const auto& reply : replies) {
            co_await send(c, reply);
            if (reply.has_desired_state()) {
                record_dispatch(session, reply);
            }
        }
        co_return true;
    }

    ruvia::task<bool> handle_node_release_request(ruvia::context& c,
        const authenticated_session& session,
        std::optional<node_release_transfer>& transfer,
        const flexedge::node::v2::ClientEnvelope& incoming) {
        const auto& request = incoming.node_release_request();
        if (!valid_node_release_request(request) || request.node_id() != session.principal_.node_id_) {
            co_await close(c.get_websocket(), {.code_ = 1008, .reason_ = "invalid node release request"});
            co_return false;
        }

        if (!transfer) {
            if (request.offset() != 0) {
                co_await close(c.get_websocket(),
                    {.code_ = 1008, .reason_ = "node release offset invalid"});
                co_return false;
            }
            const auto release = service::node_release::current();
            const auto total_bytes = release->binary().size();
            if (release->binary().digest() != request.digest_sha256() || total_bytes == 0 ||
                total_bytes > flexedge::node::maximum_node_release_bytes) {
                co_await close(c.get_websocket(), {.code_ = 1011, .reason_ = "node release unavailable"});
                co_return false;
            }
            transfer.emplace(
                node_release_transfer{.release_ = release, .digest_ = request.digest_sha256()});
        }

        const auto total_bytes = static_cast<std::uint64_t>(transfer->release_->binary().size());
        if (transfer->digest_ != request.digest_sha256() ||
            request.offset() != transfer->next_offset_ || total_bytes == 0 ||
            total_bytes > flexedge::node::maximum_node_release_bytes) {
            co_await close(c.get_websocket(),
                {.code_ = 1008, .reason_ = "node release transfer invalid"});
            co_return false;
        }
        const auto bytes = (co_await c.run_blocking([release = transfer->release_,
                                                        offset = transfer->next_offset_] {
            return release->binary().contents_range(offset, flexedge::node::node_release_chunk_bytes);
        })).value();
        if (bytes.empty()) {
            co_await close(c.get_websocket(), {.code_ = 1011, .reason_ = "node release read failed"});
            co_return false;
        }

        flexedge::node::v2::ServerEnvelope response;
        response.set_request_id(incoming.request_id());
        auto* chunk = response.mutable_node_release_chunk();
        chunk->set_version(transfer->release_->version());
        chunk->set_digest_sha256(transfer->digest_);
        chunk->set_total_bytes(total_bytes);
        chunk->set_offset(transfer->next_offset_);
        chunk->set_data(bytes);
        transfer->next_offset_ += static_cast<std::uint64_t>(bytes.size());
        if (transfer->next_offset_ == total_bytes) {
            transfer.reset();
        }
        co_await send(c, response);
        co_return true;
    }

    ruvia::task<bool> handle_apply_result(ruvia::context& c, authenticated_session& session,
        const flexedge::node::v2::ClientEnvelope& incoming) {
        const auto& result = incoming.apply_result();
        if (!valid_apply_result(result) || result.node_id() != session.principal_.node_id_) {
            co_await close(c.get_websocket(), {.code_ = 1008, .reason_ = "invalid apply result"});
            co_return false;
        }
        // Recording the trace never changes how the result is recorded.
        auto owned_trace = take_apply_trace(session, result.release_id());
        service::attempt_trace::recorder reported_trace(service::attempt_trace::trace_kind::node_apply, "web",
            reported_apply_start(result));
        auto& trace = owned_trace ? *owned_trace : reported_trace;
        import_reported_apply(trace, result);
        auto step = trace.begin_step(service::attempt_trace::step_code::record_result);
        const auto applied = result.applied() ? std::string_view{"true"} : std::string_view{"false"};
        const auto retryable = result.retryable() ? std::string_view{"true"} : std::string_view{"false"};
        const std::array fields{
            service::attempt_trace::node_message_field{.name_ = "release_id", .value_ = result.release_id()},
            service::attempt_trace::node_message_field{.name_ = "applied", .value_ = applied},
            service::attempt_trace::node_message_field{.name_ = "failed_phase",
                .value_ = reported_phase_name(result.failed_phase())},
            service::attempt_trace::node_message_field{.name_ = "error_code", .value_ = result.error_code()},
            service::attempt_trace::node_message_field{.name_ = "retryable", .value_ = retryable},
            service::attempt_trace::node_message_field{.name_ = "error", .value_ = result.error()}};
        trace.record_node_message({.direction_ = "received",
                                      .message_ = "ApplyResult",
                                      .request_id_ = incoming.request_id(),
                                      .object_count_ = 0,
                                      .bytes_ = static_cast<std::int64_t>(incoming.ByteSizeLong()),
                                      .fields_ = fields},
            std::chrono::steady_clock::now());
        co_await get_agent_command_service().record_apply_result(c, session.principal_, result,
            take_dispatch_elapsed(session, result.release_id()), trace);
        step.complete();
        if (result.applied()) {
            session.applied_node_spec_revision_ = result.node_spec_revision();
            session.active_release_id_ = result.release_id();
            session.active_manifest_digest_ = result.manifest_digest();
        }
        if (session.require_apply_ack_) {
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

    ruvia::task<bool> handle_heartbeat(ruvia::context& c, const authenticated_session& session,
        const flexedge::node::v2::ClientEnvelope& incoming) {
        if (!incoming.has_heartbeat() || !valid_heartbeat(incoming.heartbeat())) {
            co_await close(c.get_websocket(), {.code_ = 1008, .reason_ = "invalid heartbeat"});
            co_return false;
        }
        const auto& value = incoming.heartbeat();
        if (value.node_id() != session.principal_.node_id_) {
            co_await close(c.get_websocket(), {.code_ = 1008, .reason_ = "node mismatch"});
            co_return false;
        }
        co_await get_agent_command_service().heartbeat(c, session.principal_, to_heartbeat_report(value));
        flexedge::node::v2::ServerEnvelope acknowledgement;
        acknowledgement.set_request_id(incoming.request_id());
        auto* ack = acknowledgement.mutable_heartbeat_ack();
        ack->set_next_interval_seconds(session.heartbeat_interval_);
        ack->set_node_binary_sha256(service::node_release::current()->binary().digest());
        co_await send(c, acknowledgement);
        co_return true;
    }

    ruvia::task<void> serve_control_messages(ruvia::context& c, authenticated_session& session) {
        auto& ws = c.get_websocket();
        std::optional<node_release_transfer> release_transfer;
        while (const auto message = co_await read(ws)) {
            flexedge::node::v2::ClientEnvelope incoming;
            if (!parse_client_envelope(*message, incoming)) {
                co_await close(ws, {.code_ = 1003, .reason_ = "protobuf binary required"});
                co_return;
            }
            if (!valid_request_id(incoming.request_id())) {
                co_await close(ws, {.code_ = 1008, .reason_ = "invalid request id"});
                co_return;
            }
            if (incoming.has_object_request()) {
                if (!co_await handle_object_request(c, session, incoming)) {
                    co_return;
                }
                continue;
            }
            if (incoming.has_release_probe()) {
                if (!co_await handle_release_probe(c, session, incoming)) {
                    co_return;
                }
                continue;
            }
            if (incoming.has_node_release_request()) {
                if (!co_await handle_node_release_request(c, session, release_transfer, incoming)) {
                    co_return;
                }
                continue;
            }
            if (incoming.has_apply_result()) {
                if (!co_await handle_apply_result(c, session, incoming)) {
                    co_return;
                }
                continue;
            }
            if (!co_await handle_heartbeat(c, session, incoming)) {
                co_return;
            }
        }
    }

    ruvia::task<void> serve_control_session(ruvia::context& c, authenticated_session session) {
        const auto desired = co_await get_agent_read_service().desired_summary(c, session.principal_);
        flexedge::node::v2::ServerEnvelope welcome;
        welcome.set_request_id(session.request_id_);
        auto* payload = welcome.mutable_welcome();
        payload->set_node_id(session.principal_.node_id_);
        payload->set_desired_node_spec_revision(desired.node_spec_revision_);
        payload->set_desired_release_id(desired.release_id_);
        payload->set_desired_manifest_digest(desired.manifest_digest_);
        payload->set_heartbeat_interval_seconds(session.heartbeat_interval_);
        payload->set_node_binary_sha256(service::node_release::current()->binary().digest());
        co_await send(c, welcome);
        if (differs(session.applied_node_spec_revision_, session.active_release_id_,
                session.active_manifest_digest_, desired)) {
            co_await push_desired_state(c, session.request_id_, session);
        }
        co_await serve_control_messages(c, session);
    }

    ruvia::task<void> connect(ruvia::context& c) {
        try {
            co_await serve_connection(c);
        } catch (const web_socket_session_ended&) {
        }
    }

    static bool get_web_socket_session_ended(const std::error_code& error) noexcept {
        return error == asio::error::operation_aborted || error == asio::error::eof ||
               error == asio::error::connection_reset || error == asio::error::connection_aborted ||
               error == asio::error::broken_pipe;
    }

    ruvia::task<void> serve_connection(ruvia::context& c) {
        auto& ws = c.get_websocket();
        const auto first = co_await read(ws);
        flexedge::node::v2::ClientEnvelope envelope;
        if (!first || !parse_client_envelope(*first, envelope) ||
            !valid_authentication_envelope(envelope)) {
            co_await close(ws, {.code_ = 1008, .reason_ = "authentication required"});
            co_return;
        }

        auto session = co_await authenticate_session(c, envelope);
        if (session.purpose_ == flexedge::node::v2::AGENT_SESSION_PURPOSE_LOG_INGEST) {
            co_await serve_log_session(c, session);
            co_return;
        }
        co_await serve_control_session(c, std::move(session));
    }
};

}  // namespace service::agent
