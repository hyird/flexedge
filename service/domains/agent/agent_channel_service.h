#pragma once

#include <ruvia/core/task.h>

#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "node/proto/artifact.h"
#include "service/common/http.h"
#include "service/domains/agent/agent_error.h"
#include "service/domains/agent/agent_types.h"
#include "service/features/node_channel/report_queue.h"
#include "service/features/node_channel/report_waits.h"
#include "service/features/node_channel/store.h"
#include "service/features/node_dispatch/fanout.h"
#include "service/utils/secret.h"
#include "service/utils/token.h"

namespace service::agent {

// The connection has only Redis capabilities. Database persistence consumes
// immutable reports independently; the accepting Worker retains every wait
// and sends every response on its original WebSocket.
class agent_channel_service final {
public:
    template <typename context_type>
    ruvia::task<agent_principal> authenticate(context_type& context,
        const std::string& agent_id, std::string_view secret) const {
        const auto identity = co_await service::node_channel::identity(context.redis(), agent_id);
        if (!identity || !service::utils::token_hash_matches(secret, identity->secret_hash_)) {
            service::common::throw_app_error(agent_unauthorized);
        }
        agent_principal principal{.node_id_ = identity->node_id_, .cluster_id_ = identity->cluster_id_, .tenant_id_ = identity->tenant_id_, .agent_id_ = identity->agent_id_};
        flexedge::node::v2::ClientEnvelope envelope;
        envelope.mutable_authenticate()->set_node_id(agent_id);
        co_await commit_report(context, principal, std::move(envelope));
        co_return principal;
    }

    template <typename context_type>
    ruvia::task<bool> is_current(context_type& context, const agent_principal& principal) const {
        const auto identity = co_await service::node_channel::identity(context.redis(), principal.agent_id_);
        co_return identity && identity->status_ == "registered" &&
            identity->node_id_ == principal.node_id_ && identity->cluster_id_ == principal.cluster_id_ &&
            identity->tenant_id_ == principal.tenant_id_;
    }

    template <typename context_type>
    ruvia::task<flexedge::node::v2::DesiredState> desired_state(context_type& context,
        const agent_principal& principal, ruvia::stop_token stop = {}) const {
        stop = ruvia::combine_stop_tokens(context.get_stop_token(), std::move(stop));
        // Subscribe first: a projection completed while Redis is being read
        // cannot be lost between that read and the connection's next wait.
        auto changed = service::node_dispatch::fanout::get_hub(context).subscribe(context.worker(), principal.tenant_id_);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        bool repair_requested{};
        for (;;) {
            const auto envelope = co_await service::node_channel::desired(context.redis().with_options({.stop_token_ = stop}), principal);
            if (envelope) {
                if (auto result = parse_desired(*envelope, principal)) {
                    co_return std::move(*result);
                }
            }
            if (!repair_requested) {
                if (!co_await is_current(context, principal)) {
                    service::common::throw_app_error(agent_unauthorized);
                }
                (void)co_await service::node_channel::request_repair(context.redis().with_options({.stop_token_ = stop}), principal);
                repair_requested = true;
            }
            const auto remaining = deadline - std::chrono::steady_clock::now();
            if (remaining <= std::chrono::steady_clock::duration::zero()) {
                service::common::throw_app_error(revision_invalid);
            }
            const auto signal = co_await changed.receive_for(remaining, stop);
            if (!signal.has_value()) {
                service::common::throw_app_error(revision_invalid);
            }
        }
    }

    template <typename context_type>
    ruvia::task<service::agent::desired_summary> desired_summary(context_type& context,
        const agent_principal& principal, ruvia::stop_token stop = {}) const {
        const auto state = co_await desired_state(context, principal, std::move(stop));
        co_return service::agent::desired_summary{.node_spec_revision_ = state.node_spec().content().revision(),
            .release_id_ = state.release().content().release_id(),
            .manifest_digest_ = state.release().digest_sha256()};
    }

    template <typename context_type>
    ruvia::task<flexedge::node::v2::ObjectBatch> objects(context_type& context,
        const agent_principal& principal, std::string_view release_id,
        const google::protobuf::RepeatedPtrField<std::string>& requested) const {
        const std::vector<std::string> digests(requested.begin(), requested.end());
        service::node_channel::validate_object_request(digests);
        const auto state = co_await desired_state(context, principal);
        if (state.release().content().release_id() != release_id) {
            service::common::throw_app_error(revision_invalid);
        }
        for (const auto& digest : digests) {
            bool found{};
            for (const auto& object : state.release().content().objects()) {
                if (object.digest_sha256() == digest) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                service::common::throw_app_error(artifact_invalid);
            }
        }
        std::vector<std::string> records;
        bool unavailable{};
        try {
            records = co_await service::node_channel::objects(context.redis(), principal, release_id, digests);
        } catch (const service::node_channel::projection_unavailable&) {
            unavailable = true;
        }
        if (unavailable) {
            (void)co_await service::node_channel::request_repair(context.redis(), principal, release_id);
            service::common::throw_app_error(artifact_invalid);
        }
        if (records.size() != digests.size()) {
            service::common::throw_app_error(artifact_invalid);
        }
        flexedge::node::v2::ObjectBatch result;
        result.set_release_id(release_id);
        for (std::size_t index = 0; index < records.size(); ++index) {
            auto* object = result.add_objects();
            object->set_digest_sha256(digests[index]);
            bool valid{};
            try {
                service::utils::sensitive_string bytes(service::utils::open_secret(records[index]));
                valid = flexedge::node::parse_artifact(bytes.view(), *object->mutable_content()) &&
                        flexedge::node::artifact_digest(object->content()) == digests[index];
            } catch (const std::runtime_error&) {
                // Corrupt sealed bytes are repairable projection damage.
            }
            if (!valid) {
                (void)co_await service::node_channel::request_repair(context.redis(), principal, release_id);
                service::common::throw_app_error(artifact_invalid);
            }
        }
        co_return result;
    }

    template <typename context_type>
    ruvia::task<void> heartbeat(context_type& context, const agent_principal& principal,
        const flexedge::node::v2::Heartbeat& heartbeat, ruvia::stop_token stop = {}) const {
        flexedge::node::v2::ClientEnvelope envelope;
        *envelope.mutable_heartbeat() = heartbeat;
        co_await commit_report(context, principal, std::move(envelope), {}, {}, std::move(stop));
    }

    template <typename context_type>
    ruvia::task<void> record_apply_result(context_type& context, const agent_principal& principal,
        const flexedge::node::v2::ApplyResult& result, std::optional<std::int64_t> elapsed,
        service::attempt_trace::recorder& trace, ruvia::stop_token stop = {}) const {
        flexedge::node::v2::ClientEnvelope envelope;
        *envelope.mutable_apply_result() = result;
        co_await commit_report(context, principal, std::move(envelope), elapsed, trace.finish(), std::move(stop));
    }

private:
    static std::optional<flexedge::node::v2::DesiredState> parse_desired(std::string_view envelope,
        const agent_principal& principal) {
        try {
            service::utils::sensitive_string bytes(service::utils::open_secret(envelope));
            flexedge::node::v2::DesiredState result;
            if (flexedge::node::parse_artifact(bytes.view(), result) &&
                result.node_spec().content().node_id() == principal.node_id_ &&
                result.release().content().cluster_id() == principal.cluster_id_ &&
                !result.release().content().release_id().empty() &&
                flexedge::node::artifact_digest(result.node_spec().content()) == result.node_spec().digest_sha256() &&
                flexedge::node::artifact_digest(result.release().content()) == result.release().digest_sha256()) {
                return result;
            }
        } catch (const std::runtime_error&) {
            // A missing or invalid projection is rebuilt through the same
            // Redis hint; the connection never queries the source database.
        }
        return std::nullopt;
    }

    template <typename context_type>
    static ruvia::task<void> commit_report(context_type& context, const agent_principal& principal,
        flexedge::node::v2::ClientEnvelope envelope, std::optional<std::int64_t> elapsed = {},
        std::optional<service::attempt_trace::stored_trace> trace = {}, ruvia::stop_token stop = {}) {
        stop = ruvia::combine_stop_tokens(context.get_stop_token(), std::move(stop));
        const auto redis = context.redis().with_options({.stop_token_ = stop});
        const auto identity = co_await service::node_channel::identity(redis, principal.agent_id_);
        if (!identity || identity->tenant_id_ != principal.tenant_id_ || identity->node_id_ != principal.node_id_ ||
            identity->cluster_id_ != principal.cluster_id_) {
            service::common::throw_app_error(agent_unauthorized);
        }
        envelope.set_request_id(service::utils::random_token());
        service::node_channel::report report{.principal_ = principal, .verified_hash_ = identity->secret_hash_, .received_unix_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(), .envelope_ = std::move(envelope), .elapsed_milliseconds_ = elapsed, .trace_ = std::move(trace)};
        const auto id = co_await service::node_channel::enqueue(redis, report);
        if (!id) {
            throw std::runtime_error("node report queue unavailable");
        }
        auto pending = context.template worker_state<service::node_channel::report_waits>().subscribe(context.worker(), *id);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        for (;;) {
            if (const auto completed = co_await service::node_channel::completion(redis, *id)) {
                if (!*completed) {
                    service::common::throw_app_error(revision_invalid);
                }
                co_return;
            }
            const auto remaining = deadline - std::chrono::steady_clock::now();
            if (remaining <= std::chrono::steady_clock::duration::zero()) {
                throw std::runtime_error("node report persistence confirmation timed out");
            }
            const auto signal = co_await pending.receive_for(remaining, stop);
            if (!signal.has_value()) {
                throw std::runtime_error("node report persistence confirmation timed out");
            }
        }
    }
};

inline const agent_channel_service& get_agent_channel_service() {
    static const agent_channel_service service;
    return service;
}

}  // namespace service::agent
