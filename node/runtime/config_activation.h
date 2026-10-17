#pragma once

#include <chrono>
#include <cstddef>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "node/data/data_plane.h"
#include "node/runtime/apply_trace.h"
#include "node/runtime/state_store.h"

namespace flexedge::node {

// The control loop owns the only activation decision. Worker votes never compensate
// themselves, and the operation lease remains held through durable recovery.
class config_activation final {
    enum class phase { prepared,
        deciding,
        settled };

public:
    config_activation(config_activation&& other) noexcept
        : plane_(other.plane_),
          reload_(std::move(other.reload_)),
          phase_(std::exchange(other.phase_, phase::settled)) {}
    config_activation& operator=(config_activation&&) = delete;
    config_activation(const config_activation&) = delete;
    config_activation& operator=(const config_activation&) = delete;

    [[nodiscard]] static ruvia::task<config_activation> prepare(data_plane& plane,
        std::shared_ptr<const compiled_config> source,
        std::chrono::steady_clock::duration timeout = std::chrono::seconds(10)) {
        config_activation activation(plane, co_await plane.begin_reload(std::move(source), timeout));
        std::exception_ptr failure;
        try {
            co_await plane.prepare_workers(activation.reload_);
        } catch (...) {
            failure = std::current_exception();
        }
        if (failure) {
            co_await activation.compensate(failure, std::nullopt);
        }
        co_return activation;
    }

    [[nodiscard]] std::size_t added_listeners() const noexcept {
        return reload_.added_listeners();
    }

    [[nodiscard]] std::size_t retired_listeners() const noexcept {
        return reload_.retired_listeners();
    }

    ruvia::task<void> commit() {
        co_await decide(nullptr);
    }

    ruvia::task<void> commit_staged(state_store& store) {
        co_await decide(&store);
    }

private:
    struct durable_rollback final {
        state_store& store_;
        std::optional<std::string> previous_;
    };

    config_activation(data_plane& plane, data_plane_reload reload)
        : plane_(plane),
          reload_(std::move(reload)) {}

    ruvia::task<void> decide(state_store* staged) {
        if (phase_ != phase::prepared) {
            throw std::logic_error(phase_ == phase::deciding
                                       ? "configuration activation decision is already in progress"
                                       : "configuration activation is already settled");
        }
        phase_ = phase::deciding;
        std::optional<durable_rollback> durable;
        std::exception_ptr failure;
        try {
            if (staged) {
                durable.emplace(durable_rollback{*staged, staged->activate_staged()});
            }
            co_await plane_.activate_workers(reload_);
        } catch (...) {
            failure = std::current_exception();
        }
        if (failure) {
            co_await compensate(failure, std::move(durable));
        }
        plane_.commit_reload(reload_);
        phase_ = phase::settled;
    }

    ruvia::task<void> compensate(std::exception_ptr failure,
        std::optional<durable_rollback> durable) {
        try {
            co_await plane_.rollback_workers(reload_);
        } catch (...) {
            failure = std::current_exception();
        }
        try {
            if (durable) {
                durable->store_.rollback_active(durable->previous_);
            }
        } catch (...) {
            plane_.request_stop_accepting();
            phase_ = phase::settled;
            throw data_plane_unavailable("persisted configuration rollback failed");
        }
        phase_ = phase::settled;
        std::rethrow_exception(failure);
    }

    data_plane& plane_;
    data_plane_reload reload_;
    phase phase_{phase::prepared};
};

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
    auto activation = co_await config_activation::prepare(data_plane, std::move(compiled));
    co_await activation.commit();
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
    auto activation = co_await trace.run_async_step("prepare_listeners",
        [&] { return config_activation::prepare(data_plane, compiled); });
    trace.log(apply_log_level::info,
        "prepared listeners: " + std::to_string(activation.added_listeners()) + " new, " +
            std::to_string(activation.retired_listeners()) + " retired");
    phase = v2::APPLY_PHASE_ACTIVATE;
    co_await trace.run_async_step("activate", [&] { return activation.commit_staged(store); });
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
