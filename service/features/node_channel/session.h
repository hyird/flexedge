#pragma once

#include <ruvia/core/channel.h>
#include <ruvia/core/task.h>
#include <ruvia/core/task_scope.h>
#include <ruvia/web/context.h>
#include <ruvia/web/websocket.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <openssl/crypto.h>

#include "node/proto/control_protocol.h"
#include "node/proto/control_reply.h"
#include "node/proto/edge_control.pb.h"
#include "service/common/connection_error.h"
#include "service/common/http.h"
#include "service/features/attempt_trace/recorder.h"
#include "service/features/log_ingest/enqueue.h"
#include "service/features/node_channel/channel_service.h"
#include "service/features/node_channel/error.h"
#include "service/features/node_channel/protocol.h"
#include "service/features/node_channel/types.h"
#include "service/features/node_dispatch/fanout.h"
#include "service/features/node_presence/node_connection.h"
#include "service/features/node_release/catalog.h"
#include "service/features/node_release/transfer.h"
#include "service/features/sync_runtime/attempt_record.h"
#include "service/utils/sensitive_string.h"

namespace service::node_channel {

// One node's control or log WebSocket, from authentication to disconnect, on
// the Worker that accepted it.
class session final {
public:
    static ruvia::task<void> serve(ruvia::context& c) {
        try {
            co_await serve_connection(c);
        } catch (const ended&) {
        }
    }

private:
    static constexpr auto heartbeat_interval_seconds =
        static_cast<std::uint32_t>(service::node::agent_heartbeat_interval.count());

    class ended final {};

    // Serializes writes of concurrent operations on the connection.
    class response_lane final {
    public:
        explicit response_lane(const ruvia::worker_handle& worker) {
            auto [sender, receiver] = ruvia::make_channel<bool>(worker, {.capacity_ = 1});
            available_ = std::move(sender);
            waiting_.emplace(std::move(receiver));
            (void)available_.send(true);
        }
        ruvia::task<void> write(ruvia::context& context, std::string_view bytes) {
            if (!(co_await waiting_->receive(context.get_stop_token())).has_value()) {
                throw ended{};
            }
            struct release final {
                ruvia::channel_sender<bool>& sender_;
                ~release() {
                    (void)sender_.send(true);
                }
            } release_value{available_};
            co_await context.get_websocket().binary(bytes);
        }
        ruvia::task<void> close(ruvia::websocket& socket, ruvia::websocket_close_options options) {
            if (!(co_await waiting_->receive()).has_value()) {
                co_return;
            }
            struct release final {
                ruvia::channel_sender<bool>& sender_;
                ~release() {
                    (void)sender_.send(true);
                }
            } release_value{available_};
            co_await socket.close(std::move(options));
        }

    private:
        ruvia::channel_sender<bool> available_;
        std::optional<ruvia::channel_receiver<bool>> waiting_;
    };

    struct state final {
        std::string request_id_;
        agent_principal principal_;
        flexedge::node::v2::AgentSessionPurpose purpose_;
        std::int64_t applied_node_spec_revision_{};
        std::string active_release_id_;
        std::string active_manifest_digest_;
        std::uint32_t heartbeat_interval_{};
        bool require_apply_ack_{};
        bool multiplex_logs_{};
        std::unique_ptr<response_lane> responses_;
        ruvia::stop_token stop_;
        // Release of the last desired state sent on this connection and when it
        // was sent (monotonic), timing the node's next apply result for it.
        std::string dispatched_release_id_;
        std::chrono::steady_clock::time_point dispatched_at_{};
        // Trace of the node's apply of the dispatched release, owned by this
        // connection; it ends with the node's ApplyResult for that release.
        std::unique_ptr<service::attempt_trace::recorder> apply_trace_{};
    };

    static bool differs(std::int64_t node_spec_revision, std::string_view release,
        std::string_view manifest, const desired_summary& desired) {
        return node_spec_revision != desired.node_spec_revision_ || release != desired.release_id_ ||
               manifest != desired.manifest_digest_;
    }

    static ruvia::task<void> send(ruvia::context& c, const flexedge::node::v2::ServerEnvelope& envelope,
        const state& session) {
        std::string bytes;
        if (!envelope.SerializeToString(&bytes)) {
            service::common::throw_app_error(artifact_invalid);
        }
        try {
            co_await session.responses_->write(c, bytes);
        } catch (const std::system_error& error) {
            if (service::common::peer_disconnected(error.code())) {
                throw ended{};
            }
            throw;
        }
    }

    static ruvia::task<std::optional<ruvia::websocket_message>> read(ruvia::websocket& socket) {
        try {
            co_return co_await socket.read();
        } catch (const std::system_error& error) {
            if (service::common::peer_disconnected(error.code())) {
                throw ended{};
            }
            throw;
        }
    }

    static ruvia::task<void> close(ruvia::websocket& socket, ruvia::websocket_close_options options) {
        try {
            co_await socket.close(options);
        } catch (const std::system_error& error) {
            if (service::common::peer_disconnected(error.code())) {
                throw ended{};
            }
            throw;
        }
    }

    static ruvia::task<void> close_authenticated(ruvia::context& context, const state& session,
        ruvia::websocket_close_options options) {
        try {
            co_await session.responses_->close(context.get_websocket(), std::move(options));
        } catch (const std::system_error& error) {
            if (!service::common::peer_disconnected(error.code())) {
                throw;
            }
        }
    }

    static ruvia::task<void> push_desired_state(ruvia::context& c, std::string_view request_id, state& session) {
        auto desired = co_await get_channel_service().desired_state(c, session.principal_, session.stop_);
        flexedge::node::v2::ServerEnvelope response;
        response.set_request_id(request_id);
        *response.mutable_desired_state() = std::move(desired.state_);
        co_await send(c, response, session);
        record_dispatch(session, response, desired.contents_.release_);
    }

    // Times the node's next apply result for the dispatched release and starts
    // its trace. A trace that cannot be started leaves the dispatch untouched.
    static void record_dispatch(state& session, const flexedge::node::v2::ServerEnvelope& envelope,
        const flexedge::node::v2::ClusterReleaseContent& content) {
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
    static std::unique_ptr<service::attempt_trace::recorder> take_apply_trace(state& session,
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
    static std::optional<std::int64_t> take_dispatch_elapsed(state& session, std::string_view release_id) {
        if (session.dispatched_release_id_.empty() || session.dispatched_release_id_ != release_id) {
            return std::nullopt;
        }
        session.dispatched_release_id_.clear();
        const auto elapsed = service::sync_runtime::elapsed_milliseconds(session.dispatched_at_);
        session.dispatched_at_ = {};
        return elapsed;
    }

    // Validates, queues and acknowledges one log delivery, on a log session or
    // multiplexed on the control session.
    static ruvia::task<bool> ingest_logs(ruvia::context& c, const state& session,
        const flexedge::node::v2::ClientEnvelope& incoming) {
        const auto& principal = session.principal_;
        const auto& delivery = incoming.log_delivery();
        if (!service::log_ingest::valid_delivery(delivery, principal.node_id_)) {
            co_await close_authenticated(c, session, {.code_ = 1008, .reason_ = "invalid log delivery"});
            co_return false;
        }
        if (!co_await get_channel_service().is_current(c, principal)) {
            co_await close_authenticated(c, session, {.code_ = 1008, .reason_ = "agent credentials expired"});
            co_return false;
        }
        if (!co_await service::log_ingest::enqueue(c, principal.tenant_id_, principal.node_id_,
                principal.agent_id_, delivery)) {
            co_await close_authenticated(c, session, {.code_ = 1011, .reason_ = "log queue unavailable"});
            co_return false;
        }
        flexedge::node::v2::ServerEnvelope acknowledgement;
        acknowledgement.set_request_id(incoming.request_id());
        acknowledgement.mutable_log_delivery_ack();
        co_await send(c, acknowledgement, session);
        co_return true;
    }

    static ruvia::task<state> authenticate(ruvia::context& c, flexedge::node::v2::ClientEnvelope& envelope) {
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
        auto principal = co_await get_channel_service().authenticate(c, agent_id, secret.view());
        co_return state{
            .request_id_ = envelope.request_id(),
            .principal_ = std::move(principal),
            .purpose_ = purpose,
            .applied_node_spec_revision_ = applied_node_spec_revision,
            .active_release_id_ = active_release_id,
            .active_manifest_digest_ = active_manifest_digest,
            .heartbeat_interval_ = heartbeat_interval_seconds,
            .require_apply_ack_ = authentication.require_apply_ack(),
            .multiplex_logs_ = authentication.multiplex_logs(),
            .responses_ = std::make_unique<response_lane>(c.worker()),
            .stop_ = c.get_stop_token(),
            .dispatched_release_id_ = {},
            .dispatched_at_ = {},
        };
    }

    // The separate log session of nodes that do not multiplex logs.
    static ruvia::task<void> serve_log_session(ruvia::context& c, const state& session) {
        flexedge::node::v2::ServerEnvelope welcome;
        welcome.set_request_id(session.request_id_);
        welcome.mutable_welcome()->set_node_id(session.principal_.node_id_);
        welcome.mutable_welcome()->set_node_binary_sha256(service::node_release::current(c)->binary().digest());
        co_await send(c, welcome, session);
        auto& ws = c.get_websocket();
        while (const auto message = co_await read(ws)) {
            flexedge::node::v2::ClientEnvelope incoming;
            if (!parse_client_envelope(*message, incoming) || !valid_request_id(incoming.request_id()) ||
                !incoming.has_log_delivery()) {
                co_await close_authenticated(c, session, {.code_ = 1003, .reason_ = "log delivery required"});
                co_return;
            }
            if (!co_await ingest_logs(c, session, incoming)) {
                co_return;
            }
        }
    }

    static ruvia::task<bool> handle_object_request(ruvia::context& c, const state& session,
        const flexedge::node::v2::ClientEnvelope& incoming) {
        const auto& request = incoming.object_request();
        if (!valid_object_request(request) || request.node_id() != session.principal_.node_id_) {
            co_await close_authenticated(c, session, {.code_ = 1008, .reason_ = "invalid object request"});
            co_return false;
        }
        const auto received_at = std::chrono::steady_clock::now();
        auto objects = co_await get_channel_service().objects(
            c, session.principal_, request.release_id(), request.digest_sha256());
        flexedge::node::v2::ServerEnvelope response;
        response.set_request_id(incoming.request_id());
        *response.mutable_object_batch() = std::move(objects);
        co_await send(c, response, session);
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

    static ruvia::task<bool> handle_release_probe(ruvia::context& c, state& session,
        const flexedge::node::v2::ClientEnvelope& incoming) {
        const auto& probe = incoming.release_probe();
        if (probe.wait_seconds() > flexedge::node::release_watch_maximum_seconds) {
            co_await close_authenticated(c, session, {.code_ = 1008, .reason_ = "invalid watch deadline"});
            co_return false;
        }
        // Subscribe before reading: a commit between the query and wait remains queued.
        auto subscription = service::node_dispatch::fanout::get_hub(c).subscribe(
            c.worker(), session.principal_.tenant_id_);
        auto desired = co_await get_channel_service().desired_summary(c, session.principal_, session.stop_);
        const auto known_revision = probe.applied_node_spec_revision() > 0
                                        ? probe.applied_node_spec_revision()
                                        : session.applied_node_spec_revision_;
        const auto known_release = probe.applied_node_spec_revision() > 0
                                       ? probe.active_release_id()
                                       : session.active_release_id_;
        const auto known_digest = probe.applied_node_spec_revision() > 0
                                      ? probe.active_manifest_digest()
                                      : session.active_manifest_digest_;
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(probe.wait_seconds());
        while (!differs(known_revision, known_release, known_digest, desired)) {
            const auto remaining = deadline - std::chrono::steady_clock::now();
            if (remaining <= std::chrono::steady_clock::duration::zero()) {
                break;
            }
            const auto signal = co_await subscription.receive_for(remaining, session.stop_);
            if (!signal.has_value() && signal.status() != ruvia::worker_wait_status::timed_out) {
                co_return false;
            }
            desired = co_await get_channel_service().desired_summary(c, session.principal_, session.stop_);
        }
        std::optional<desired_delivery> delivery;
        if (differs(known_revision, known_release, known_digest, desired)) {
            delivery = co_await get_channel_service().desired_state(c, session.principal_, session.stop_);
            // One snapshot supplies both frames, even if a newer release commits meanwhile.
            desired = {.node_spec_revision_ = delivery->contents_.node_spec_.revision(),
                .release_id_ = delivery->contents_.release_.release_id(),
                .manifest_digest_ = delivery->state_.release().digest_sha256()};
        }
        const auto replies = flexedge::node::release_probe_replies(incoming.request_id(),
            service::node_release::current(c)->binary().digest(), desired.node_spec_revision_,
            desired.release_id_, desired.manifest_digest_,
            delivery ? std::optional<flexedge::node::v2::DesiredState>(std::move(delivery->state_)) : std::nullopt);
        for (const auto& reply : replies) {
            co_await send(c, reply, session);
            if (reply.has_desired_state()) {
                record_dispatch(session, reply, delivery->contents_.release_);
            }
        }
        co_return true;
    }

    static ruvia::task<bool> handle_node_release_request(ruvia::context& c, const state& session,
        service::node_release::transfer& transfer, const flexedge::node::v2::ClientEnvelope& incoming) {
        const auto& request = incoming.node_release_request();
        if (!valid_node_release_request(request) || request.node_id() != session.principal_.node_id_) {
            co_await close_authenticated(c, session, {.code_ = 1008, .reason_ = "invalid node release request"});
            co_return false;
        }
        auto outcome = co_await transfer.next(c, request);
        if (!outcome.chunk_) {
            switch (outcome.error_) {
                case service::node_release::transfer_error::offset_invalid:
                    co_await close_authenticated(c, session, {.code_ = 1008, .reason_ = "node release offset invalid"});
                    break;
                case service::node_release::transfer_error::unavailable:
                    co_await close_authenticated(c, session, {.code_ = 1011, .reason_ = "node release unavailable"});
                    break;
                case service::node_release::transfer_error::transfer_invalid:
                    co_await close_authenticated(c, session, {.code_ = 1008, .reason_ = "node release transfer invalid"});
                    break;
                case service::node_release::transfer_error::read_failed:
                    co_await close_authenticated(c, session, {.code_ = 1011, .reason_ = "node release read failed"});
                    break;
            }
            co_return false;
        }
        flexedge::node::v2::ServerEnvelope response;
        response.set_request_id(incoming.request_id());
        *response.mutable_node_release_chunk() = std::move(*outcome.chunk_);
        co_await send(c, response, session);
        co_return true;
    }

    static ruvia::task<bool> handle_apply_result(ruvia::context& c, state& session,
        const flexedge::node::v2::ClientEnvelope& incoming) {
        const auto& result = incoming.apply_result();
        if (!valid_apply_result(result) || result.node_id() != session.principal_.node_id_) {
            co_await close_authenticated(c, session, {.code_ = 1008, .reason_ = "invalid apply result"});
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
        step.complete();
        co_await get_channel_service().record_apply_result(c, session.principal_, result,
            take_dispatch_elapsed(session, result.release_id()), trace, session.stop_);
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
            co_await send(c, reply, session);
        }
        co_return true;
    }

    static ruvia::task<bool> handle_heartbeat(ruvia::context& c, const state& session,
        const flexedge::node::v2::ClientEnvelope& incoming) {
        if (!incoming.has_heartbeat() || !valid_heartbeat(incoming.heartbeat())) {
            co_await close_authenticated(c, session, {.code_ = 1008, .reason_ = "invalid heartbeat"});
            co_return false;
        }
        const auto& value = incoming.heartbeat();
        if (value.node_id() != session.principal_.node_id_) {
            co_await close_authenticated(c, session, {.code_ = 1008, .reason_ = "node mismatch"});
            co_return false;
        }
        co_await get_channel_service().heartbeat(c, session.principal_, value, session.stop_);
        flexedge::node::v2::ServerEnvelope acknowledgement;
        acknowledgement.set_request_id(incoming.request_id());
        auto* ack = acknowledgement.mutable_heartbeat_ack();
        ack->set_next_interval_seconds(session.heartbeat_interval_);
        ack->set_node_binary_sha256(service::node_release::current(c)->binary().digest());
        co_await send(c, acknowledgement, session);
        co_return true;
    }

    static ruvia::task<void> run_release_watch(ruvia::context& c, state& session,
        flexedge::node::v2::ClientEnvelope incoming, ruvia::task_scope& operations, bool& watching,
        std::exception_ptr& failure) {
        try {
            if (!co_await handle_release_probe(c, session, incoming) && !session.stop_.stop_requested()) {
                throw ended{};
            }
        } catch (...) {
            if (!session.stop_.stop_requested()) {
                failure = std::current_exception();
            }
            operations.request_stop();
            c.get_websocket().abort();
        }
        watching = false;
    }

    static ruvia::task<void> read_control_messages(ruvia::context& c, state& session,
        ruvia::task_scope& operations, bool& watching, std::exception_ptr& failure) {
        auto& ws = c.get_websocket();
        service::node_release::transfer release_transfer;
        while (const auto message = co_await read(ws)) {
            flexedge::node::v2::ClientEnvelope incoming;
            if (!parse_client_envelope(*message, incoming)) {
                co_await close_authenticated(c, session, {.code_ = 1003, .reason_ = "protobuf binary required"});
                co_return;
            }
            if (!valid_request_id(incoming.request_id())) {
                co_await close_authenticated(c, session, {.code_ = 1008, .reason_ = "invalid request id"});
                co_return;
            }
            if (incoming.has_object_request()) {
                if (!co_await handle_object_request(c, session, incoming)) {
                    co_return;
                }
                continue;
            }
            if (incoming.has_release_probe()) {
                if (watching || incoming.release_probe().wait_seconds() > flexedge::node::release_watch_maximum_seconds) {
                    co_await close_authenticated(c, session, {.code_ = 1008, .reason_ = "invalid concurrent watch"});
                    co_return;
                }
                watching = true;
                operations.spawn(run_release_watch(c, session, std::move(incoming), operations, watching, failure));
                continue;
            }
            if (incoming.has_log_delivery()) {
                if (!session.multiplex_logs_ || !co_await ingest_logs(c, session, incoming)) {
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

    static ruvia::task<void> serve_control_messages(ruvia::context& c, state& session) {
        ruvia::task_scope operations(c.worker());
        session.stop_ = ruvia::combine_stop_tokens(c.get_stop_token(), operations.get_stop_token());
        bool watching{};
        std::exception_ptr failure;
        try {
            co_await read_control_messages(c, session, operations, watching, failure);
        } catch (...) {
            if (!failure) {
                failure = std::current_exception();
            }
        }
        operations.request_stop();
        c.get_websocket().abort();
        co_await operations.join();
        if (failure) {
            std::rethrow_exception(failure);
        }
    }

    static ruvia::task<void> serve_control_session(ruvia::context& c, state session) {
        const auto desired = co_await get_channel_service().desired_summary(c, session.principal_, session.stop_);
        flexedge::node::v2::ServerEnvelope welcome;
        welcome.set_request_id(session.request_id_);
        auto* payload = welcome.mutable_welcome();
        payload->set_node_id(session.principal_.node_id_);
        payload->set_desired_node_spec_revision(desired.node_spec_revision_);
        payload->set_desired_release_id(desired.release_id_);
        payload->set_desired_manifest_digest(desired.manifest_digest_);
        payload->set_heartbeat_interval_seconds(session.heartbeat_interval_);
        payload->set_node_binary_sha256(service::node_release::current(c)->binary().digest());
        payload->set_multiplex_logs(session.multiplex_logs_);
        co_await send(c, welcome, session);
        if (differs(session.applied_node_spec_revision_, session.active_release_id_,
                session.active_manifest_digest_, desired)) {
            co_await push_desired_state(c, session.request_id_, session);
        }
        co_await serve_control_messages(c, session);
    }

    static ruvia::task<void> serve_connection(ruvia::context& c) {
        auto& ws = c.get_websocket();
        const auto first = co_await read(ws);
        flexedge::node::v2::ClientEnvelope envelope;
        if (!first || !parse_client_envelope(*first, envelope) || !valid_authentication_envelope(envelope)) {
            co_await close(ws, {.code_ = 1008, .reason_ = "authentication required"});
            co_return;
        }
        auto session = co_await authenticate(c, envelope);
        if (session.purpose_ == flexedge::node::v2::AGENT_SESSION_PURPOSE_LOG_INGEST) {
            co_await serve_log_session(c, session);
            co_return;
        }
        co_await serve_control_session(c, std::move(session));
    }
};

}  // namespace service::node_channel
