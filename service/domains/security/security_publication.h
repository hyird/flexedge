#pragma once
#include <ruvia/web/context.h>

#include <set>
#include <span>
#include <string>

#include "service/domains/website/website_reference.h"
#include "service/features/node_dispatch/queue.h"
#include "service/features/sync_runtime/state.h"
#include "service/features/sync_runtime/work_commit.h"
namespace service::security {
inline ruvia::task<void> publish_policy_changes(ruvia::db_transaction& tx, ruvia::context& c,
    const std::string& tenant, std::span<const std::string> policies) {
    service::background::queued_work work;
    service::live_resource::notifications::resource_notification notice(tenant);
    std::set<std::string> clusters;
    for (const auto& policy : policies) {
        const auto sites = co_await service::website::revise_security_policy_websites(tx, tenant, policy);
        for (const auto& site : sites) {
            clusters.insert(site.cluster_id_);
            (void)co_await service::sync_runtime::upsert_marker(tx, tenant, service::sync_runtime::marker_resource_type::website, site.id_, service::sync_runtime::marker_operation::apply, site.revision_, work);
            notice.changed(service::live_resource::resource::websites, site.id_);
        }
    }
    for (const auto& cluster : clusters) {
        co_await service::node_dispatch::publish_cluster_release(tx, tenant, cluster, work);
    }
    if (!clusters.empty()) {
        notice.node_config();
    }
    co_await service::sync_runtime::commit_work(tx, c, std::move(notice), work);
}
}  // namespace service::security
