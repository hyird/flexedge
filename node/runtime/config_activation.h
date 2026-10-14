#pragma once

#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "node/data/data_plane.h"
#include "node/runtime/state_store.h"

namespace flexedge::node {

// A persisted state that no longer compiles is an unusable local cache; listener and
// activation failures keep their original errors.
inline void activate_persisted_config(data_plane& data_plane, const v2::ActiveState& active,
    const std::vector<v2::DeliveryObject>& objects) {
    auto state = std::make_shared<v2::ActiveState>(active);
    std::shared_ptr<const compiled_config> compiled;
    try {
        compiled = std::make_shared<const compiled_config>(state, objects);
    } catch (const std::exception& error) {
        throw stored_state_unusable(std::string("stored active state does not compile: ") +
                                    error.what());
    }
    auto reload = data_plane.prepare(compiled);
    data_plane.prime(reload);
    data_plane.activate(std::move(reload));
}

inline void activate_desired_config(state_store& store, data_plane& data_plane, const v2::DesiredState& desired, const std::vector<v2::DeliveryObject>& objects,
    v2::ApplyPhase& phase) {
    v2::ActiveState active;
    *active.mutable_node_spec() = desired.node_spec();
    *active.mutable_release() = desired.release();
    phase = v2::APPLY_PHASE_STAGE;
    store.stage(active, objects);
    phase = v2::APPLY_PHASE_VALIDATE;
    auto state = std::make_shared<v2::ActiveState>(active);
    auto compiled = std::make_shared<const compiled_config>(state, objects);
    auto reload = data_plane.prepare(compiled);
    phase = v2::APPLY_PHASE_ACTIVATE;
    data_plane.prime(reload);
    try {
        store.activate_staged();
    } catch (...) {
        data_plane.abort(reload);
        throw;
    }
    data_plane.activate(std::move(reload));
}

}  // namespace flexedge::node
