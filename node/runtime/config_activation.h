#pragma once

#include <memory>
#include <utility>
#include <vector>
#include "node/data/data_plane.h"
#include "node/runtime/state_store.h"

namespace flexedge::node {

inline void activatePersistedConfig(DataPlane& dataPlane, const v2::ActiveState& active,
                    const std::vector<v2::DeliveryObject>& objects) {
    auto state = std::make_shared<v2::ActiveState>(active);
    auto compiled = std::make_shared<const CompiledConfig>(state, objects);
    auto reload = dataPlane.prepare(compiled);
    dataPlane.prime(reload);
    dataPlane.activate(std::move(reload));
}

inline void activateDesiredConfig(StateStore& store, DataPlane& dataPlane, const v2::DesiredState& desired, const std::vector<v2::DeliveryObject>& objects,
           v2::ApplyPhase& phase) {
    v2::ActiveState active;
    *active.mutable_node_spec() = desired.node_spec();
    *active.mutable_release() = desired.release();
    phase = v2::APPLY_PHASE_STAGE;
    store.stage(active, objects);
    phase = v2::APPLY_PHASE_VALIDATE;
    auto state = std::make_shared<v2::ActiveState>(active);
    auto compiled = std::make_shared<const CompiledConfig>(state, objects);
    auto reload = dataPlane.prepare(compiled);
    phase = v2::APPLY_PHASE_ACTIVATE;
    dataPlane.prime(reload);
    try {
        store.activateStaged();
    } catch (...) {
        dataPlane.abort(reload);
        throw;
    }
    dataPlane.activate(std::move(reload));

}

} // namespace flexedge::node
