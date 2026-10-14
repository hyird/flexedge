#pragma once

#include <algorithm>
#include <exception>
#include <string_view>
#include <vector>

namespace service::dns {

enum class record_reconciliation_action { reuse,
    update,
    create };

template <typename record_type>
struct record_reconciliation_plan final {
    record_reconciliation_action action_;
    const record_type* record_;
};

// Provider clients own transport and provider-specific equality. The decision of whether a
// known remote record can be reused, updated, or must be created is shared by every provider.
template <typename record_type, typename matches_identity_type, typename matches_desired_type, typename conflict_type>
[[nodiscard]] inline record_reconciliation_plan<record_type>
plan_record_reconciliation(const std::vector<record_type>& remote_records,
    std::string_view known_remote_record_id, matches_identity_type&& matches_identity,
    matches_desired_type&& matches_desired, conflict_type&& conflict) {
    if (!known_remote_record_id.empty()) {
        const auto by_id = std::find_if(
            remote_records.begin(), remote_records.end(),
            [known_remote_record_id](const auto& record) { return record.id_ == known_remote_record_id; });
        if (by_id != remote_records.end()) {
            return {matches_desired(*by_id) ? record_reconciliation_action::reuse
                                            : record_reconciliation_action::update,
                &*by_id};
        }
    }

    const record_type* exact = nullptr;
    for (const auto& record : remote_records) {
        if (!matches_identity(record)) {
            continue;
        }
        if (exact != nullptr && exact->id_ != record.id_) {
            conflict();
            std::terminate();
        }
        exact = &record;
    }
    if (exact != nullptr) {
        return {matches_desired(*exact) ? record_reconciliation_action::reuse
                                        : record_reconciliation_action::update,
            exact};
    }
    return {record_reconciliation_action::create, nullptr};
}

}  // namespace service::dns
