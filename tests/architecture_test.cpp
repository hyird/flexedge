#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <ruvia/web/ModelJson.h>

#include "service/common/domain_name.h"
#include "service/common/http.h"
#include "service/common/ip_address.h"
#include "service/config/outbound.h"
#include "service/config/schema.h"
#include "service/domains/overview/overview.types.h"
#include "service/features/certificate/worker.h"
#include "service/features/dns/registry.h"
#include "service/features/dns_sync/worker.h"
#include "service/features/log_ingest/tail.h"
#include "service/features/node_dispatch/protocol.h"
#include "service/features/provider_verification/worker.h"
#include "service/features/sync_runtime/state.h"
#include "service/features/website_dispatch/worker.h"
#include "service/features/website_dns/model.h"
#include "service/features/website_dns/runtime.h"
#include "service/features/node_release/artifact.h"
#include "service/utils/secret.h"
#include "service/utils/sensitive_string.h"
#include "node/data/origin_selection.h"
#include "node/data/origin_health.h"
#include "node/data/route_rules.h"

namespace {

template <typename Function> bool throwsRuntimeError(Function&& function) {
    try {
        function();
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

std::string source(std::string_view relativePath) {
    std::ifstream input(std::filesystem::path(FLEXEDGE_SOURCE_DIR) / relativePath,
                        std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot read architecture source: " + std::string(relativePath));
    }
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

} // namespace

#define REQUIRE(condition)                                                                         \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            throw std::runtime_error("requirement failed: " #condition);                           \
        }                                                                                          \
    } while (false)

int main() {
    REQUIRE(service::sync_runtime::resourceColumn("provider") == "provider_id");
    REQUIRE(service::sync_runtime::resourceColumn("dns_zone") == "dns_zone_id");
    REQUIRE(service::sync_runtime::resourceColumn("certificate") == "certificate_id");
    REQUIRE(service::sync_runtime::resourceColumn("website") == "website_id");
    REQUIRE(throwsRuntimeError([] { (void)service::sync_runtime::resourceColumn("cluster"); }));

    static_assert(service::dns::kDnsProviders.size() == 2);
    static_assert(service::dns::findDnsProvider("cloudflare")->supportsProxy);
    static_assert(service::dns::findDnsProvider("aliyun")->supportsRoutingLines);
    static_assert(service::dns::findDnsProvider("unknown") == nullptr);
    static_assert(service::node_dispatch::canReportAppliedNodeSpecRevision(2, 2, 3));
    static_assert(service::node_dispatch::canReportAppliedNodeSpecRevision(2, 3, 3));
    static_assert(!service::node_dispatch::canReportAppliedNodeSpecRevision(3, 2, 4));
    static_assert(!service::node_dispatch::canReportAppliedNodeSpecRevision(2, 4, 3));

    const auto configuredOrigins = service::config::makeOutboundOrigins(std::string{});
    REQUIRE(&service::config::outboundOriginConfig(configuredOrigins,
                                                   service::config::kCloudflareOriginAlias) ==
            &configuredOrigins.cloudflare);
    REQUIRE(&service::config::outboundOriginConfig(configuredOrigins,
                                                   service::config::kAliyunDnsOriginAlias) ==
            &configuredOrigins.aliyunDns);

    const auto cursor = service::log_ingest::parseTailCursor(
        "1787776027394526:b2d6e77d-4a0c-4796-93d5-755d3c6d3837");
    REQUIRE(cursor);
    REQUIRE(service::log_ingest::encodeTailCursor(cursor->ingestedUnixMicros, cursor->id) ==
            "1787776027394526:b2d6e77d-4a0c-4796-93d5-755d3c6d3837");
    REQUIRE(!service::log_ingest::parseTailCursor("0:not-a-uuid"));
    REQUIRE(service::common::certificateCoversHostname("*.example.com", "www.example.com"));
    REQUIRE(!service::common::certificateCoversHostname("*.example.com", "a.b.example.com"));
    REQUIRE(service::common::domainBelongsToZone("www.example.com", "example.com"));
    REQUIRE(service::common::isHostname("*.example.com"));
    REQUIRE(!service::common::isHostname("-invalid.example.com"));
    REQUIRE(service::common::isIpAddress("2001:db8::1"));
    REQUIRE(!service::common::isIpv4Address("2001:db8::1"));

    service::utils::SensitiveString secret(std::string("first-secret"));
    service::utils::SensitiveString moved(std::move(secret));
    REQUIRE(secret.view().empty());
    REQUIRE(moved.view() == "first-secret");
    service::utils::configureSecretKey(std::string(64, 'a'));
    const auto sealed = service::utils::sealSecret("sensitive-value");
    REQUIRE(sealed.starts_with("v1."));
    REQUIRE(service::utils::openSecret(sealed) == "sensitive-value");
    auto tampered = sealed;
    tampered.back() = tampered.back() == 'a' ? 'b' : 'a';
    REQUIRE(throwsRuntimeError([&] { (void)service::utils::openSecret(tampered); }));

    service::overview::OverviewResourceCountsDto resources;
    resources.set<"websiteCount">(2);
    resources.set<"domainCount">(3);
    resources.set<"certificateCount">(4);
    resources.set<"clusterCount">(1);
    service::overview::OverviewIssueCountsDto issues;
    issues.set<"dnsZoneIssueCount">(1);
    issues.set<"certificateExpiringCount">(2);
    issues.set<"certificateFailedCount">(0);
    issues.set<"activeTaskCount">(3);
    issues.set<"failedTaskCount">(1);
    service::overview::OverviewDataDto overview;
    overview.set<"resources">(std::move(resources));
    overview.set<"issues">(std::move(issues));
    (void)overview.ensure<"recentTasks">();
    REQUIRE(ruvia::toJson(overview).contains("\"recent_tasks\":[]"));

    const auto sourceRoot = std::filesystem::path(FLEXEDGE_SOURCE_DIR);
    REQUIRE(!std::filesystem::exists(sourceRoot / "service/features/task_runtime/state.h"));
    REQUIRE(!std::filesystem::exists(sourceRoot / "service/features/certificate/task_log.h"));
    REQUIRE(
        !std::filesystem::exists(sourceRoot / "service/features/node_dispatch/task_completion.h"));
    REQUIRE(!std::filesystem::exists(sourceRoot / "service/features/node_dispatch/worker.h"));
    REQUIRE(!std::filesystem::exists(sourceRoot / "service/features/website_dispatch/queue.h"));
    REQUIRE(!std::filesystem::exists(sourceRoot / "service/features/website_dispatch/model.h"));

    REQUIRE(service::config::kSchemaMigrations.size() == 17);
    REQUIRE(service::config::kSchemaMigrations.back().id() == "0017_website_route_rules");
    const auto syncMigration =
        std::ranges::find_if(service::config::kSchemaMigrations, [](const auto& migration) {
            return migration.id() == "0015_edgeadmin_sync_markers";
        });
    REQUIRE(syncMigration != service::config::kSchemaMigrations.end());
    REQUIRE(syncMigration->sql().contains("CREATE TABLE public.sys_sync_task"));
    REQUIRE(syncMigration->sql().contains("uk_sync_task_resource"));
    REQUIRE(syncMigration->sql().contains("DROP TABLE IF EXISTS public.sys_task"));
    REQUIRE(syncMigration->sql().contains("version bigint NOT NULL"));
    REQUIRE(syncMigration->sql().contains("processed_version bigint DEFAULT 0 NOT NULL"));
    REQUIRE(syncMigration->sql().contains("lease_until timestamptz"));
    REQUIRE(syncMigration->sql().contains(
        "num_nonnulls(provider_id, dns_zone_id, certificate_id, website_id) = 1"));
    const auto infiniteRetryMigration =
        std::ranges::find_if(service::config::kSchemaMigrations, [](const auto& migration) {
            return migration.id() == "0016_sync_task_infinite_retry";
        });
    REQUIRE(infiniteRetryMigration != service::config::kSchemaMigrations.end());
    REQUIRE(infiniteRetryMigration->sql().contains("is_done = FALSE"));
    REQUIRE(service::config::kSchemaMigrations.back().sql().contains("\"route_rules\": []"));

    const auto syncRuntime = source("service/features/sync_runtime/state.h");
    REQUIRE(syncRuntime.contains("namespace service::sync_runtime"));
    REQUIRE(syncRuntime.contains("ON CONFLICT (tenant_id, resource_type, "));
    REQUIRE(syncRuntime.contains("resource_id) DO UPDATE SET"));
    REQUIRE(syncRuntime.contains("GREATEST(sys_sync_task.version, "));
    REQUIRE(syncRuntime.contains("EXCLUDED.version), operation"));
    REQUIRE(syncRuntime.contains("renewRunningLease"));
    REQUIRE(syncRuntime.contains("completeRunning"));
    REQUIRE(syncRuntime.contains("removeRunning"));
    REQUIRE(syncRuntime.contains("recoverStaleRunning"));
    REQUIRE(!syncRuntime.contains("kMaximumFailures"));

    const auto taskService = source("service/domains/task/task.service.h");
    REQUIRE(taskService.contains("WITH marker_view AS"));
    REQUIRE(taskService.contains("FROM sys_sync_task marker"));
    REQUIRE(taskService.contains("marker.updated_at, marker.tenant_id"));
    REQUIRE(!taskService.contains("parent_task_id"));
    REQUIRE(!taskService.contains("spec_snapshot"));
    const auto taskTypes = source("service/domains/task/task.types.h");
    REQUIRE(taskTypes.contains("processed_version"));
    REQUIRE(taskTypes.contains("count_fails"));
    REQUIRE(!taskTypes.contains("children"));
    REQUIRE(!taskTypes.contains("parent_task_id"));
    const auto taskController = source("service/domains/task/task.controller.h");
    REQUIRE(taskController.contains("\"pending\", \"running\", \"retry\""));
    REQUIRE(!taskController.contains("\"waiting\""));

    const auto nodeDispatch = source("service/features/node_dispatch/queue.h");
    REQUIRE(nodeDispatch.contains("publishClusterRelease"));
    REQUIRE(nodeDispatch.contains("sys_node_release_target"));
    REQUIRE(!nodeDispatch.contains("sys_task"));
    REQUIRE(!nodeDispatch.contains("parent_task_id"));
    const auto agentService = source("service/domains/agent/agent.service.h");
    REQUIRE(!agentService.contains("reconcileReleaseTask"));
    REQUIRE(!agentService.contains("sys_task"));
    const auto server = source("service/server.cpp");
    REQUIRE(!server.contains("node-dispatch"));
    REQUIRE(server.contains("website-dispatch"));

    for (const auto* path :
         {"service/features/provider_verification/worker.h", "service/features/dns_sync/worker.h",
          "service/features/certificate/worker.h", "service/features/website_dispatch/worker.h"}) {
        const auto worker = source(path);
        REQUIRE(worker.contains("sys_sync_task"));
        REQUIRE(worker.contains("recoverStaleRunning"));
        REQUIRE(!worker.contains("sys_task"));
        REQUIRE(!worker.contains("task_runtime"));
        REQUIRE(!worker.contains("spec_snapshot"));
    }
    const auto dnsSnapshot = source("service/features/dns_sync/snapshot.h");
    REQUIRE(dnsSnapshot.contains("challenge_records"));
    const auto dnsChallenge = source("service/features/certificate/dns_challenge.h");
    REQUIRE(dnsChallenge.contains("RunningMarkerLease"));
    REQUIRE(!dnsChallenge.contains("RunningTaskLease"));
    const auto websiteRuntime = source("service/features/website_dns/runtime.h");
    REQUIRE(websiteRuntime.contains("sync_runtime::completeRunning"));
    REQUIRE(!websiteRuntime.contains("task_runtime"));

    flexedge::node::v2::Website routeWebsite;
    routeWebsite.set_default_origin_group("default");
    auto* fallbackRoute = routeWebsite.add_route_rules();
    fallbackRoute->set_id("route-root");
    fallbackRoute->set_enabled(true);
    fallbackRoute->set_match_type("prefix");
    fallbackRoute->set_path("/");
    fallbackRoute->set_action("proxy");
    fallbackRoute->set_origin_group("default");
    auto* route = routeWebsite.add_route_rules();
    route->set_id("route-1");
    route->set_enabled(true);
    route->set_match_type("prefix");
    route->set_path("/api");
    route->add_methods("GET");
    route->set_action("proxy");
    route->set_rewrite_path("/internal");
    route->set_origin_group("api");
    auto* defaultOrigin = routeWebsite.add_origins();
    defaultOrigin->set_id("origin-default");
    defaultOrigin->set_group("default");
    defaultOrigin->set_protocol("http");
    defaultOrigin->set_host("127.0.0.1");
    defaultOrigin->set_port(80);
    defaultOrigin->set_role("primary");
    defaultOrigin->set_weight(100);
    defaultOrigin->set_enabled(true);
    auto* routeOrigin = routeWebsite.add_origins();
    routeOrigin->set_id("origin-route");
    routeOrigin->set_group("api");
    routeOrigin->set_protocol("http");
    routeOrigin->set_host("127.0.0.2");
    routeOrigin->set_port(80);
    routeOrigin->set_role("primary");
    routeOrigin->set_weight(100);
    routeOrigin->set_enabled(true);
    auto* requestHeader = route->add_request_headers();
    requestHeader->set_name("X-Route");
    requestHeader->set_value("enabled");
    auto* responseHeader = route->add_response_headers();
    responseHeader->set_name("X-Edge-Route");
    responseHeader->set_value("api");
    flexedge::node::validateRouteRules(routeWebsite);
    const auto* matched = flexedge::node::matchedRouteRule(routeWebsite, "GET", "/api/v1?q=1");
    REQUIRE(matched == route);
    REQUIRE(flexedge::node::matchedRouteRule(routeWebsite, "GET", "/apix") == fallbackRoute);
    REQUIRE(flexedge::node::routeTarget("/api/v1?q=1", matched) == "/internal?q=1");
    const auto routeOrigins = flexedge::node::originCandidates(routeWebsite, 0, matched);
    REQUIRE(routeOrigins.size() == 1);
    REQUIRE(routeOrigins.front()->id() == "origin-route");
    const auto defaultOrigins = flexedge::node::originCandidates(routeWebsite, 0);
    REQUIRE(defaultOrigins.size() == 1);
    REQUIRE(defaultOrigins.front()->id() == "origin-default");
    REQUIRE(flexedge::node::matchedRouteRule(routeWebsite, "POST", "/api/v1") == fallbackRoute);
    std::vector<std::pair<std::string, std::string>> routeHeaders{{"X-Route", "old"},
                                                                  {"Accept", "application/json"}};
    flexedge::node::applyRouteHeaders(routeHeaders, route->request_headers());
    REQUIRE(routeHeaders.size() == 2);
    REQUIRE(routeHeaders.front().first == "Accept");
    REQUIRE(routeHeaders.back().first == "X-Route");
    REQUIRE(routeHeaders.back().second == "enabled");
    flexedge::node::OriginHealthRegistry originHealth;
    originHealth.recordProbe("website-1", "origin-1", false, 1, 42, "timeout");
    const auto healthReports = originHealth.reports();
    REQUIRE(healthReports.size() == 1);
    REQUIRE(healthReports.front().status == "unhealthy");
    REQUIRE(healthReports.front().latencyMillis == 42);
    REQUIRE(healthReports.front().lastError == "timeout");
    auto* invalidRoute = routeWebsite.add_route_rules();
    invalidRoute->set_id("route-2");
    invalidRoute->set_match_type("exact");
    invalidRoute->set_path("/");
    invalidRoute->set_action("proxy");
    auto* invalidHeader = invalidRoute->add_request_headers();
    invalidHeader->set_name("Content-Length");
    invalidHeader->set_value("0");
    REQUIRE(throwsRuntimeError([&] { flexedge::node::validateRouteRules(routeWebsite); }));

    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(sourceRoot / "service")) {
        if (!entry.is_regular_file() || entry.path().filename() == "schema.h") {
            continue;
        }
        const auto content =
            source(std::filesystem::relative(entry.path(), sourceRoot).generic_string());
        REQUIRE(!content.contains("sys_task"));
        REQUIRE(!content.contains("task_runtime"));
        REQUIRE(!content.contains("parent_task_id"));
        REQUIRE(!content.contains("spec_snapshot"));
    }
    return 0;
}
