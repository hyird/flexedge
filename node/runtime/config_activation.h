#pragma once

#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "node/data/data_plane.h"
#include "node/runtime/apply_trace.h"
#include "node/runtime/state_store.h"

namespace flexedge::node {

// A persisted state that no longer compiles is an unusable local cache; listener and
// activation failures keep their original errors.
inline ruvia::task<void> activate_persisted_config(data_plane& data_plane, const v2::ActiveState& active,
    const std::vector<v2::DeliveryObject>& objects) {
    std::shared_ptr<const compiled_config> compiled;
    try {
        compiled = std::make_shared<const compiled_config>(active, objects);
    } catch (const std::exception& error) {
        throw stored_state_unusable(std::string("stored active state does not compile: ") +
                                    error.what());
    }
    auto reload = co_await data_plane.prepare(compiled);
    co_await data_plane.activate(reload);
}

// Disk and worker activation form one transaction. Worker rollback finishes before the
// previous encrypted state is restored, and no failed attempt is reported as applied.
inline ruvia::task<void> activate_staged_config(state_store& store, data_plane& data_plane,
    prepared_data_plane_reload reload) {
    std::optional<std::string> previous;
    bool persisted{};
    std::exception_ptr failure;
    try {
        previous = store.activate_staged();
        persisted = true;
        co_await data_plane.activate(reload);
    } catch (...) {
        failure = std::current_exception();
    }
    if (failure) {
        try {
            co_await data_plane.rollback(reload);
        } catch (...) {
            failure = std::current_exception();
        }
        try {
            if (persisted) {
                store.rollback_active(previous);
            }
        } catch (...) {
            data_plane.request_stop_accepting();
            throw data_plane_unavailable("persisted configuration rollback failed");
        }
        std::rethrow_exception(failure);
    }
}

// Stages, validates, prepares and activates `desired`, recording each step in `trace`. `phase`
// is the ApplyPhase reached, reported as failed_phase when a step throws.
inline ruvia::task<void> activate_desired_config(state_store& store, data_plane& data_plane, const v2::DesiredState& desired, const std::vector<v2::DeliveryObject>& objects,
    v2::ApplyPhase& phase, apply_trace& trace) {
    v2::ActiveState active;
    *active.mutable_node_spec() = desired.node_spec();
    *active.mutable_release() = desired.release();
    phase = v2::APPLY_PHASE_STAGE;
    trace.run_step("stage", [&] { store.stage(active, objects); });
    trace.log(apply_log_level::info, "staged the release state");
    phase = v2::APPLY_PHASE_VALIDATE;
    auto compiled = trace.run_step("validate",
        [&] { return std::make_shared<const compiled_config>(active, objects); });
    trace.log(apply_log_level::info,
        "compiled the configuration: " + std::to_string(compiled->websites().size()) +
            " websites, " + std::to_string(compiled->endpoints().size()) + " endpoints" +
            (compiled->enabled() ? "" : ", node disabled"));
    auto reload = co_await trace.run_async_step("prepare_listeners", [&] { return data_plane.prepare(compiled); });
    trace.log(apply_log_level::info,
        "prepared listeners: " + std::to_string(reload.added_listeners()) + " new, " +
            std::to_string(reload.retired_listeners()) + " retired");
    phase = v2::APPLY_PHASE_ACTIVATE;
    co_await trace.run_async_step("activate", [&] { return activate_staged_config(store, data_plane, std::move(reload)); });
    trace.log(apply_log_level::info, "activated the configuration");
}

// Applies `desired` and fills `result` with the outcome, the steps and the log lines in
// `trace`. Returns the failure, rethrown once the control plane has confirmed the result.
inline ruvia::task<std::exception_ptr> apply_desired_config(state_store& store, data_plane& data_plane,
    const v2::DesiredState& desired, const std::vector<v2::DeliveryObject>& objects,
    apply_trace& trace, v2::ApplyResult& result) {
    v2::ApplyPhase phase = v2::APPLY_PHASE_STAGE;
    std::exception_ptr failure;
    try {
        co_await activate_desired_config(store, data_plane, desired, objects, phase, trace);
        result.set_applied(true);
        result.set_failed_phase(v2::APPLY_PHASE_UNSPECIFIED);
    } catch (const data_plane_unavailable&) {
        // Assembly must join the failed data plane; reconnecting cannot repair an exited
        // worker. The staged activation already restored its durable state before this.
        throw;
    } catch (const std::exception& error) {
        result.set_applied(false);
        result.set_failed_phase(phase);
        result.set_error_code(phase == v2::APPLY_PHASE_VALIDATE ? "artifact_invalid" : "apply_failed");
        result.set_error(error.what());
        result.set_retryable(phase != v2::APPLY_PHASE_VALIDATE);
        trace.log(apply_log_level::error, std::string("apply failed: ") + error.what());
        failure = std::current_exception();
    }
    trace.write_to(result);
    co_return failure;
}

}  // namespace flexedge::node
