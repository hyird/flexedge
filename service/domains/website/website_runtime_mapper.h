#pragma once

#include <ruvia/web/context.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "service/common/domain_name.h"
#include "service/domains/website/website_types.h"
#include "service/features/node_runtime/model.h"
#include "service/features/website_config/model.h"
#include "service/features/website_dns/model.h"

namespace service::website::detail {

struct bound_certificate final {
    std::string id_;
    std::vector<std::string> domains_;
    bool usable_;
};

struct origin_runtime_state final {
    std::string node_id_;
    std::string node_name_;
    service::node_runtime::node_runtime_data::origin_health health_;
};

inline website_runtime_dto to_runtime(auto& c,
    const service::website_config::website_config_data& config,
    const service::website_dns::website_runtime_data& input,
    const std::vector<bound_certificate>& certificates,
    std::int64_t target_count, std::int64_t synced_count,
    const std::vector<origin_runtime_state>& origin_states = {}) {
    website_runtime_dto output({.resource_ = c.pool()});
    const auto deploy_status = target_count == 0              ? std::string_view{"no_nodes"}
                               : synced_count == target_count ? std::string_view{"applied"}
                               : synced_count == 0            ? std::string_view{"pending"}
                                                              : std::string_view{"partial"};
    output.template set<"deploy_status">(deploy_status);
    output.template set<"target_node_count">(target_count);
    output.template set<"synced_node_count">(synced_count);
    auto& domains = output.template ensure<"domain_states">();
    for (const auto& domain : config.domains_) {
        const auto state = std::ranges::find_if(input.domain_states_, [&](const auto& candidate) {
            return candidate.id_ && *candidate.id_ == domain.id_;
        });
        const auto* runtime_state = state == input.domain_states_.end() ? nullptr : &*state;
        const bool https =
            config.https_enabled_ && std::ranges::any_of(certificates, [&](const auto& certificate) {
                return certificate.usable_ &&
                       std::ranges::any_of(certificate.domains_, [&](const auto& certificate_domain) {
                           return service::common::certificate_covers_hostname(certificate_domain,
                               domain.hostname_);
                       });
            });
        auto& item = domains.emplace_back();
        const auto resolution_status = runtime_state && runtime_state->resolution_status_
                                           ? std::string_view(*runtime_state->resolution_status_)
                                           : std::string_view{"unverified"};
        item.template set<"id">(domain.id_);
        item.template set<"access_protocol">(https ? std::string_view{"https"} : std::string_view{"http"});
        item.template set<"resolution_status">(resolution_status);
        if (runtime_state && runtime_state->last_verified_at_) {
            item.template set<"last_verified_at">(*runtime_state->last_verified_at_);
        }
        if (runtime_state && runtime_state->last_error_) {
            item.template set<"last_error">(*runtime_state->last_error_);
        }
    }
    auto& origins = output.template ensure<"origin_states">();
    origins.reserve(origin_states.size());
    for (const auto& state : origin_states) {
        auto& item = origins.emplace_back();
        item.template set<"node_id">(state.node_id_);
        item.template set<"node_name">(state.node_name_);
        item.template set<"origin_id">(state.health_.origin_id_);
        item.template set<"status">(state.health_.status_);
        item.template set<"checked_at_unix_millis">(state.health_.checked_at_unix_millis_);
        item.template set<"latency_millis">(state.health_.latency_millis_);
        if (state.health_.last_error_) {
            item.template set<"last_error">(*state.health_.last_error_);
        }
    }
    return output;
}

}  // namespace service::website::detail
