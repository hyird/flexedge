#pragma once

#include <algorithm>
#include <exception>
#include <string_view>
#include <vector>

namespace service::dns {

enum class RecordReconciliationAction { reuse, update, create };

template <typename Record> struct RecordReconciliationPlan final {
    RecordReconciliationAction action;
    const Record* record;
};

// Provider clients own transport and provider-specific equality. The decision of whether a
// known remote record can be reused, updated, or must be created is shared by every provider.
template <typename Record, typename MatchesIdentity, typename MatchesDesired, typename Conflict>
[[nodiscard]] inline RecordReconciliationPlan<Record>
planRecordReconciliation(const std::vector<Record>& remoteRecords,
                         std::string_view knownRemoteRecordId, MatchesIdentity&& matchesIdentity,
                         MatchesDesired&& matchesDesired, Conflict&& conflict) {
    if (!knownRemoteRecordId.empty()) {
        const auto byId = std::find_if(
            remoteRecords.begin(), remoteRecords.end(),
            [knownRemoteRecordId](const auto& record) { return record.id == knownRemoteRecordId; });
        if (byId != remoteRecords.end()) {
            return {matchesDesired(*byId) ? RecordReconciliationAction::reuse
                                          : RecordReconciliationAction::update,
                    &*byId};
        }
    }

    const Record* exact = nullptr;
    for (const auto& record : remoteRecords) {
        if (!matchesIdentity(record)) {
            continue;
        }
        if (exact != nullptr && exact->id != record.id) {
            conflict();
            std::terminate();
        }
        exact = &record;
    }
    if (exact != nullptr) {
        return {matchesDesired(*exact) ? RecordReconciliationAction::reuse
                                       : RecordReconciliationAction::update,
                exact};
    }
    return {RecordReconciliationAction::create, nullptr};
}

} // namespace service::dns
