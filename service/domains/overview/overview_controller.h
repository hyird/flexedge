#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/context.h>
#include <ruvia/web/controller.h>
#include <ruvia/web/model_json.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "service/common/http.h"
#include "service/domains/node/node_read_service.h"
#include "service/domains/overview/overview_service.h"
#include "service/domains/overview/overview_traffic_service.h"
#include "service/features/live_resource/fanout.h"
#include "service/features/live_resource/sse.h"
#include "service/middleware/auth.h"

namespace service::overview {

class overview_controller final {
public:
    RUVIA_CONTROLLER_GROUP("/api/overview", service::middleware::auth_middleware)
    RUVIA_ROUTES_BEGIN(overview_controller)
    RUVIA_GET_SSE("/stream", get);
    RUVIA_ROUTES_END

private:
    // The overview page keeps one connection. Each event type is a separate
    // projection with its own event source, cache key and single-flight read:
    // `snapshot` (counts and markers), `traffic` (access-log aggregate,
    // coalesced), `nodes` (the first node list page) with its `node-runtime`
    // patches. A change re-reads only the projection it affects.
    static constexpr std::int64_t node_page = 1;
    static constexpr std::int64_t node_page_size = 100;

    ruvia::task<void> get(ruvia::context& c) {
        namespace live = service::live_resource;
        const auto tenant = service::middleware::current_tenant_id(c);
        std::vector<live::subscription_part> parts;
        parts.push_back({live::resource::overview, {}, live::query_key("overview")});
        parts.push_back({live::resource::overview_traffic, {}, live::query_key("traffic")});
        // Same key as `/api/nodes/stream?page=1&page_size=100`: one shared read.
        parts.push_back({live::resource::nodes, {},
            service::node::node_list_query_key(node_page, node_page_size, "",
                std::nullopt, std::nullopt,
                std::nullopt, std::nullopt)});
        auto sub = live::get_hub(c).subscribe(c.worker(), tenant, std::move(parts));

        std::vector<live::snapshot_part> snapshots;
        snapshots.push_back({"snapshot", [tenant](auto& read) -> ruvia::task<std::string> {
                                 auto data = co_await get_overview_service().get(read, tenant);
                                 co_return std::string(ruvia::to_json(
                                     service::common::ok<overview_response>(read, std::move(data)),
                                     {.resource_ = read.pool()}));
                             }});
        snapshots.push_back({"traffic", [tenant](auto& read) -> ruvia::task<std::string> {
                                 auto data = co_await get_overview_traffic_service().get(read, tenant);
                                 co_return std::string(ruvia::to_json(
                                     service::common::ok<overview_traffic_response>(read, std::move(data)),
                                     {.resource_ = read.pool()}));
                             }});
        snapshots.push_back({"nodes", [tenant](auto& read) -> ruvia::task<std::string> {
                                 auto data = co_await service::node::get_node_read_service().list(
                                     read, tenant, node_page, node_page_size, 0, std::nullopt, std::nullopt,
                                     std::nullopt, std::nullopt, std::nullopt);
                                 co_return std::string(ruvia::to_json(
                                     service::common::ok<service::node::node_page_response>(read, std::move(data)),
                                     {.resource_ = read.pool()}));
                             }});
        co_await live::stream_snapshots(c, std::move(sub), std::move(snapshots));
    }
};

}  // namespace service::overview
