#include <algorithm>
#include <cctype>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
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
#include "service/features/dns/record_reconciliation.h"
#include "service/features/dns/registry.h"
#include "service/features/dns_sync/worker.h"
#include "service/features/log_ingest/tail.h"
#include "service/features/node_dispatch/protocol.h"
#include "service/features/provider_verification/worker.h"
#include "service/features/sync_runtime/error.h"
#include "service/features/sync_runtime/state.h"
#include "service/features/website_dispatch/worker.h"
#include "service/features/website_dns/model.h"
#include "service/features/website_dns/runtime.h"
#include "service/features/node_release/artifact.h"
#include "service/utils/secret.h"
#include "service/utils/sensitive_string.h"
#include "service/utils/token.h"
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
            std::cerr << "requirement failed: " #condition << '\n';                                \
            throw std::runtime_error("requirement failed: " #condition);                           \
        }                                                                                          \
    } while (false)

int main() {
    using service::sync_runtime::MarkerOperation;
    using service::sync_runtime::MarkerResourceType;
    REQUIRE(service::sync_runtime::resourceTypeName(MarkerResourceType::provider) == "provider");
    REQUIRE(service::sync_runtime::resourceTypeName(MarkerResourceType::dnsZone) == "dns_zone");
    REQUIRE(service::sync_runtime::resourceTypeName(MarkerResourceType::certificate) ==
            "certificate");
    REQUIRE(service::sync_runtime::resourceTypeName(MarkerResourceType::website) == "website");
    REQUIRE(service::sync_runtime::resourceColumn(MarkerResourceType::provider) == "provider_id");
    REQUIRE(service::sync_runtime::resourceColumn(MarkerResourceType::dnsZone) == "dns_zone_id");
    REQUIRE(service::sync_runtime::resourceColumn(MarkerResourceType::certificate) ==
            "certificate_id");
    REQUIRE(service::sync_runtime::resourceColumn(MarkerResourceType::website) == "website_id");
    REQUIRE(service::sync_runtime::markerOperationName(MarkerOperation::verify) == "verify");
    REQUIRE(service::sync_runtime::markerOperationName(MarkerOperation::sync) == "sync");
    REQUIRE(service::sync_runtime::markerOperationName(MarkerOperation::syncLocal) == "sync_local");
    REQUIRE(service::sync_runtime::markerOperationName(MarkerOperation::syncRemote) ==
            "sync_remote");
    REQUIRE(service::sync_runtime::markerOperationName(MarkerOperation::remove) == "delete");
    REQUIRE(service::sync_runtime::markerOperationName(MarkerOperation::issue) == "issue");
    REQUIRE(service::sync_runtime::markerOperationName(MarkerOperation::renew) == "renew");
    REQUIRE(service::sync_runtime::markerOperationName(MarkerOperation::apply) == "apply");
    REQUIRE(service::sync_runtime::supportsMarkerOperation(MarkerResourceType::dnsZone,
                                                           MarkerOperation::syncRemote));
    REQUIRE(!service::sync_runtime::supportsMarkerOperation(MarkerResourceType::website,
                                                            MarkerOperation::remove));
    REQUIRE(throwsRuntimeError([] {
        (void)service::sync_runtime::resourceTypeName(static_cast<MarkerResourceType>(99));
    }));
    REQUIRE(throwsRuntimeError(
        [] { (void)service::sync_runtime::resourceColumn(static_cast<MarkerResourceType>(99)); }));
    REQUIRE(
        throwsRuntimeError([] { (void)service::sync_runtime::requireMarkerOperation("unknown"); }));
    const auto markerLease =
        service::sync_runtime::makeRunningLease("tenant", "marker", 7, "worker");
    REQUIRE(markerLease.marker.tenantId == "tenant");
    REQUIRE(markerLease.marker.markerId == "marker");
    REQUIRE(markerLease.marker.version == 7);
    REQUIRE(markerLease.owner == "worker");
    REQUIRE(throwsRuntimeError(
        [] { (void)service::sync_runtime::makeRunningLease("tenant", "marker", 0, "worker"); }));

    static_assert(service::dns::kDnsProviders.size() == 2);
    static_assert(service::dns::findDnsProvider("cloudflare")->supportsProxy);
    static_assert(service::dns::findDnsProvider("aliyun")->supportsRoutingLines);
    static_assert(service::dns::findDnsProvider("unknown") == nullptr);
    struct ReconciliationTestRecord final {
        std::string id;
        bool desired;
    };
    const std::vector<ReconciliationTestRecord> reconciliationRecords{
        {"known", false},
        {"exact", true},
    };
    const auto knownRecordPlan = service::dns::planRecordReconciliation(
        reconciliationRecords, "known", [](const ReconciliationTestRecord&) { return false; },
        [](const ReconciliationTestRecord& record) { return record.desired; },
        [] { throw std::runtime_error("unexpected duplicate"); });
    REQUIRE(knownRecordPlan.action == service::dns::RecordReconciliationAction::update);
    REQUIRE(knownRecordPlan.record->id == "known");
    const auto exactRecordPlan = service::dns::planRecordReconciliation(
        reconciliationRecords, "missing",
        [](const ReconciliationTestRecord& record) { return record.id == "exact"; },
        [](const ReconciliationTestRecord& record) { return record.desired; },
        [] { throw std::runtime_error("unexpected duplicate"); });
    REQUIRE(exactRecordPlan.action == service::dns::RecordReconciliationAction::reuse);
    REQUIRE(exactRecordPlan.record->id == "exact");
    const auto newRecordPlan = service::dns::planRecordReconciliation(
        reconciliationRecords, "missing", [](const ReconciliationTestRecord&) { return false; },
        [](const ReconciliationTestRecord&) { return false; },
        [] { throw std::runtime_error("unexpected duplicate"); });
    REQUIRE(newRecordPlan.action == service::dns::RecordReconciliationAction::create);
    REQUIRE(newRecordPlan.record == nullptr);
    REQUIRE(throwsRuntimeError([&] {
        (void)service::dns::planRecordReconciliation(
            reconciliationRecords, "missing",
            [](const ReconciliationTestRecord& record) {
                return record.id == "exact" || record.id == "known";
            },
            [](const ReconciliationTestRecord&) { return false; },
            [] { throw std::runtime_error("duplicate record"); });
    }));
    const auto dnsDriver = source("service/features/dns/driver.h");
    REQUIRE(dnsDriver.contains("planRecordReconciliation"));
    REQUIRE(!dnsDriver.contains("std::vector<CloudflareRecord> records"));
    REQUIRE(!dnsDriver.contains("std::vector<AliyunRecord> records"));
    REQUIRE(!source("service/features/dns/cloudflare.h").contains("reconcileRecord"));
    REQUIRE(!source("service/features/dns/aliyun.h").contains("reconcileRecord"));
    const auto dnsSyncReconciliation = source("service/features/dns_sync/reconciliation.h");
    REQUIRE(dnsSyncReconciliation.contains("requiresManualResolution"));
    REQUIRE(dnsSyncReconciliation.contains("expected.content != remote.content"));
    REQUIRE(service::dns_sync::recordHostname("test", "a-z.xin") == "test.a-z.xin");
    REQUIRE(service::dns_sync::recordHostname("test.a-z.xin", "a-z.xin") == "test.a-z.xin");
    REQUIRE(service::dns_sync::recordHostname("@", "a-z.xin") == "a-z.xin");
    REQUIRE(service::dns_sync::isTrafficRecord("CNAME"));
    REQUIRE(!service::dns_sync::isTrafficRecord("TXT"));
    const auto dnsZoneService = source("service/domains/dns_zone/dns_zone.service.h");
    REQUIRE(dnsZoneService.contains("dns_zone_read.service.h"));
    REQUIRE(dnsZoneService.contains("dnsZoneReadService().options"));
    REQUIRE(dnsZoneService.contains("dnsZoneReadService().available"));
    REQUIRE(dnsZoneService.contains("dnsZoneReadService().list"));
    REQUIRE(dnsZoneService.contains("dnsZoneReadService().get"));
    REQUIRE(!dnsZoneService.contains("selectColumns"));
    REQUIRE(!dnsZoneService.contains("fillDnsZone"));
    REQUIRE(dnsZoneService.contains("validateSystemManagedRecords"));
    REQUIRE(dnsZoneService.contains("SYSTEM_MANAGED_RECORD"));
    const auto dnsZoneReadService = source("service/domains/dns_zone/dns_zone_read.service.h");
    REQUIRE(dnsZoneReadService.contains("class DnsZoneReadService final"));
    REQUIRE(dnsZoneReadService.contains("SELECT zone.id, provider.id"));
    REQUIRE(dnsZoneReadService.contains("parseDnsProviderRuntime"));
    REQUIRE(dnsZoneReadService.contains("loadProjectedRecordsByZone"));
    const auto clusterService = source("service/domains/cluster/cluster.service.h");
    REQUIRE(clusterService.contains("cluster_read.service.h"));
    REQUIRE(clusterService.contains("clusterReadService().list"));
    REQUIRE(!clusterService.contains("SELECT cluster.id, cluster.name"));
    REQUIRE(clusterService.contains("publishClusterRelease"));
    REQUIRE(clusterService.contains("reconcileZoneTransition"));
    const auto clusterReadService = source("service/domains/cluster/cluster_read.service.h");
    REQUIRE(clusterReadService.contains("class ClusterReadService final"));
    REQUIRE(clusterReadService.contains("SELECT cluster.id, cluster.name"));
    REQUIRE(clusterReadService.contains("onlineNodeCount"));
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

    const auto firstToken = service::utils::randomToken();
    const auto secondToken = service::utils::randomToken();
    REQUIRE(firstToken.size() == 64);
    REQUIRE(std::ranges::all_of(firstToken, [](unsigned char ch) { return std::isxdigit(ch); }));
    REQUIRE(firstToken != secondToken);

    service::overview::OverviewResourceCountsDto resources;
    resources.set<"websiteCount">(2);
    resources.set<"domainCount">(3);
    resources.set<"certificateCount">(4);
    resources.set<"clusterCount">(1);
    service::overview::OverviewIssueCountsDto issues;
    issues.set<"dnsZoneIssueCount">(1);
    issues.set<"certificateExpiringCount">(2);
    issues.set<"certificateFailedCount">(0);
    issues.set<"activeMarkerCount">(3);
    issues.set<"retryMarkerCount">(1);
    service::overview::OverviewDataDto overview;
    overview.set<"resources">(std::move(resources));
    overview.set<"issues">(std::move(issues));
    (void)overview.ensure<"recentMarkers">();
    REQUIRE(ruvia::toJson(overview).contains("\"recent_markers\":[]"));

    const auto sourceRoot = std::filesystem::path(FLEXEDGE_SOURCE_DIR);
    REQUIRE(!std::filesystem::exists(sourceRoot / "service/features/task_runtime/state.h"));
    REQUIRE(!std::filesystem::exists(sourceRoot / "service/features/certificate/task_log.h"));
    REQUIRE(
        !std::filesystem::exists(sourceRoot / "service/features/node_dispatch/task_completion.h"));
    REQUIRE(!std::filesystem::exists(sourceRoot / "service/features/node_dispatch/worker.h"));
    REQUIRE(!std::filesystem::exists(sourceRoot / "service/features/website_dispatch/queue.h"));
    REQUIRE(!std::filesystem::exists(sourceRoot / "service/features/website_dispatch/model.h"));

    const auto& schemaMigrations = service::config::kSchemaMigrations;
    REQUIRE(schemaMigrations.size() >= 20);
    REQUIRE(schemaMigrations.front().id() == "0001");
    REQUIRE(schemaMigrations.back().id() == "0020_sync_result_events");
    const auto& syncEventMigration = schemaMigrations.back().sql();
    REQUIRE(syncEventMigration.starts_with("DO $flexedge_sync_result_events$"));
    REQUIRE(syncEventMigration.contains("CREATE TABLE public.sys_sync_event"));
    const auto& schemaBaseline = schemaMigrations.front().sql();
    // The production database records this checksum. Never fold later schema
    // work into migration 0001 after it has been released.
    REQUIRE(schemaBaseline.contains("CREATE TABLE public.sys_task"));
    REQUIRE(schemaBaseline.contains("node_key_hash"));
    REQUIRE(schemaBaseline.contains("device_key_fingerprint"));
    REQUIRE(!schemaBaseline.contains("sys_sync_event"));
    REQUIRE(!schemaBaseline.contains("DROP TABLE"));
    REQUIRE(!schemaBaseline.contains("migrate_data"));
    std::string schemaSql;
    for (const auto& migration : schemaMigrations) {
        schemaSql += migration.sql();
    }
    REQUIRE(schemaSql.contains("RENAME COLUMN node_key_envelope TO node_secret_envelope"));
    REQUIRE(schemaSql.contains("RENAME COLUMN protocol_version TO schema_version"));
    REQUIRE(schemaSql.contains("CREATE TABLE public.sys_sync_task"));
    REQUIRE(schemaSql.contains("CREATE TABLE public.sys_sync_event"));
    REQUIRE(schemaSql.contains("uk_sync_task_resource"));
    REQUIRE(schemaSql.contains("idx_sync_event_tenant_id"));
    REQUIRE(schemaSql.contains("version bigint NOT NULL"));
    REQUIRE(schemaSql.contains("processed_version bigint DEFAULT 0 NOT NULL"));
    REQUIRE(schemaSql.contains("lease_until timestamptz"));
    REQUIRE(schemaSql.contains(
        "num_nonnulls(provider_id, dns_zone_id, certificate_id, website_id) = 1"));
    REQUIRE(schemaSql.contains("request_bytes bigint DEFAULT 0 NOT NULL"));
    REQUIRE(schemaSql.contains("idx_website_access_log_website_ingested"));
    REQUIRE(schemaSql.contains("idx_node_log_node_ingested"));

    const auto websiteConfig = source("service/features/website_config/model.h");
    REQUIRE(websiteConfig.contains("!defaultOriginGroup"));
    REQUIRE(websiteConfig.contains("!healthCheckPath"));
    REQUIRE(websiteConfig.contains("!healthyThreshold"));
    REQUIRE(websiteConfig.contains("!group || !protocol"));
    REQUIRE(!websiteConfig.contains("legacyRouteRules"));
    REQUIRE(websiteConfig.contains("website_config/transport.h"));
    REQUIRE(!websiteConfig.contains("RUVIA_REQUEST_MODEL(WebsiteDomainInput"));
    const auto websiteConfigTransport = source("service/features/website_config/transport.h");
    REQUIRE(websiteConfigTransport.contains("RUVIA_REQUEST_MODEL(WebsiteDomainInput"));
    REQUIRE(websiteConfigTransport.contains("WebsiteConfigOutput, RUVIA_OPTIONAL_FIELD(name"));
    REQUIRE(!std::filesystem::exists(sourceRoot / "service/domains/website/website.service.h"));
    const auto websiteCommand = source("service/domains/website/website_command.service.h");
    REQUIRE(websiteCommand.contains("class WebsiteCommandService final"));
    REQUIRE(websiteCommand.contains("requestDnsProbe"));
    REQUIRE(websiteCommand.contains("replaceRelationProjections"));
    REQUIRE(websiteCommand.contains("website_dns::reconcileConfigChange"));
    REQUIRE(websiteCommand.contains("node_dispatch::publishClusterRelease"));
    REQUIRE(!websiteCommand.contains("website_read.service.h"));
    REQUIRE(!websiteCommand.contains("website_access_log.service.h"));
    REQUIRE(!websiteCommand.contains("website_dashboard.service.h"));
    REQUIRE(!websiteCommand.contains("minute_buckets AS"));
    const auto websiteDashboard = source("service/domains/website/website_dashboard.service.h");
    REQUIRE(websiteDashboard.contains("class WebsiteDashboardService final"));
    REQUIRE(websiteDashboard.contains("minute_buckets AS"));
    const auto websiteAccessLogMapper =
        source("service/domains/website/website_access_log.mapper.h");
    REQUIRE(websiteAccessLogMapper.contains("fillWebsiteAccessLog"));
    REQUIRE(websiteAccessLogMapper.contains("clientIpLocation"));
    REQUIRE(!websiteCommand.contains("sys_website_access_log"));
    REQUIRE(!websiteCommand.contains("static WebsiteRuntimeDto toRuntime"));
    REQUIRE(!websiteCommand.contains("static void fillWebsite"));
    const auto websiteRuntimeMapper = source("service/domains/website/website_runtime.mapper.h");
    REQUIRE(websiteRuntimeMapper.contains("inline WebsiteRuntimeDto toRuntime"));
    REQUIRE(websiteRuntimeMapper.contains("struct BoundCertificate final"));
    const auto websiteReadService = source("service/domains/website/website_read.service.h");
    REQUIRE(websiteReadService.contains("class WebsiteReadService final"));
    REQUIRE(websiteReadService.contains("website_runtime.mapper.h"));
    REQUIRE(websiteReadService.contains("loadBoundCertificatesByWebsite"));
    REQUIRE(websiteReadService.contains("loadOriginRuntime"));
    const auto websiteAccessLogs = source("service/domains/website/website_access_log.service.h");
    REQUIRE(websiteAccessLogs.contains("class WebsiteAccessLogService final"));
    REQUIRE(websiteAccessLogs.contains("sys_website_access_log access LEFT JOIN"));
    REQUIRE(websiteAccessLogs.contains("website_access_log.mapper.h"));
    const auto acme = source("service/features/certificate/acme.h");
    REQUIRE(acme.contains("certificate/acme_types.h"));
    REQUIRE(acme.contains("certificate/acme_jws.h"));
    REQUIRE(acme.contains("certificate/certificate_inspection.h"));
    REQUIRE(acme.contains("certificate/certificate_request.h"));
    REQUIRE(!acme.contains("inline std::string hmacSha256"));
    REQUIRE(!acme.contains("inline std::string jwkThumbprint"));
    REQUIRE(!acme.contains("inline IssuedCertificate inspectCertificate"));
    REQUIRE(!acme.contains("inline CertificateRequest createCertificateRequest"));
    REQUIRE(!acme.contains("ensureAcmeAccount"));
    REQUIRE(!acme.contains("parseCertificateProviderRuntime"));
    const auto acmeJws = source("service/features/certificate/acme_jws.h");
    REQUIRE(acmeJws.contains("inline std::string hmacSha256"));
    REQUIRE(acmeJws.contains("inline std::string jwkThumbprint"));
    REQUIRE(acmeJws.contains("inline EvpPkeyPtr generateRsaKey"));
    const auto certificateInspection =
        source("service/features/certificate/certificate_inspection.h");
    REQUIRE(certificateInspection.contains("inline IssuedCertificate inspectCertificate"));
    REQUIRE(certificateInspection.contains("inline void validateCertificateDomains"));
    const auto certificateRequest = source("service/features/certificate/certificate_request.h");
    REQUIRE(certificateRequest.contains("inline CertificateRequest createCertificateRequest"));
    const auto certificateService = source("service/domains/certificate/certificate.service.h");
    REQUIRE(certificateService.contains("certificate_read.service.h"));
    REQUIRE(certificateService.contains("certificateReadService().list"));
    REQUIRE(certificateService.contains("certificateReadService().get"));
    REQUIRE(certificateService.contains("certificateReadService().download"));
    REQUIRE(!certificateService.contains("static void fillCertificate"));
    REQUIRE(!certificateService.contains("certificateColumns"));
    REQUIRE(!certificateService.contains("buildCertificateArchive"));
    REQUIRE(!certificateService.contains("deflateForZip"));
    const auto certificateMapper = source("service/domains/certificate/certificate.mapper.h");
    REQUIRE(certificateMapper.contains("inline void fillCertificate"));
    const auto certificateReadService =
        source("service/domains/certificate/certificate_read.service.h");
    REQUIRE(certificateReadService.contains("class CertificateReadService final"));
    REQUIRE(certificateReadService.contains("certificate.mapper.h"));
    REQUIRE(certificateReadService.contains("certificateColumns"));
    REQUIRE(certificateReadService.contains("buildArchive"));
    const auto certificateDownload = source("service/features/certificate_material/download.h");
    REQUIRE(certificateDownload.contains("inline std::string archiveFilename"));
    REQUIRE(certificateDownload.contains("struct CertificateDownload final"));
    REQUIRE(certificateService.contains("certificate_material/download.h"));
    REQUIRE(!certificateService.contains("struct CertificateDownload final"));
    REQUIRE(!certificateService.contains("static std::string certificateFilename"));
    const auto certificateArchive = source("service/features/certificate_material/archive.h");
    REQUIRE(certificateArchive.contains("inline std::string buildArchive"));
    REQUIRE(certificateArchive.contains("namespace detail"));
    REQUIRE(certificateArchive.contains("deflateForZip"));
    REQUIRE(!std::filesystem::exists(sourceRoot / "service/domains/agent/agent.service.h"));
    const auto agentCommand = source("service/domains/agent/agent_command.service.h");
    REQUIRE(agentCommand.contains("class AgentCommandService final"));
    REQUIRE(agentCommand.contains("agent.types.h"));
    REQUIRE(!agentCommand.contains("agent_read.service.h"));
    REQUIRE(!agentCommand.contains("agentReadService()"));
    REQUIRE(!agentCommand.contains("struct HeartbeatReport final"));
    REQUIRE(!agentCommand.contains("sys_cluster_release_object mapping"));
    const auto agentTypes = source("service/domains/agent/agent.types.h");
    REQUIRE(agentTypes.contains("struct HeartbeatReport final"));
    const auto agentReadService = source("service/domains/agent/agent_read.service.h");
    REQUIRE(agentReadService.contains("class AgentReadService final"));
    REQUIRE(agentReadService.contains("sys_cluster_release_object mapping"));
    REQUIRE(agentReadService.contains("artifactDigest"));
    REQUIRE(agentCommand.contains("agent_runtime.mapper.h"));
    REQUIRE(!agentCommand.contains("NodeRuntimeOutput runtime"));
    const auto agentRuntimeMapper = source("service/domains/agent/agent_runtime.mapper.h");
    REQUIRE(agentRuntimeMapper.contains("inline std::string heartbeatRuntimeJson"));
    const auto agentController = source("service/domains/agent/agent.controller.h");
    REQUIRE(agentController.contains("agent_protocol.h"));
    REQUIRE(agentController.contains("agent_command.service.h"));
    REQUIRE(agentController.contains("agent_read.service.h"));
    REQUIRE(!agentController.contains("agent.service.h"));
    REQUIRE(agentController.contains("agentCommandService().authenticate"));
    REQUIRE(agentController.contains("agentReadService().desiredSummary"));
    REQUIRE(!agentController.contains("static bool validHeartbeat"));
    REQUIRE(!agentController.contains("static bool parseEnvelope"));
    REQUIRE(!agentController.contains("static HeartbeatReport report"));
    const auto agentProtocol = source("service/domains/agent/agent_protocol.h");
    REQUIRE(agentProtocol.contains("inline bool parseClientEnvelope"));
    REQUIRE(agentProtocol.contains("inline bool validHeartbeat"));
    REQUIRE(agentProtocol.contains("inline HeartbeatReport toHeartbeatReport"));
    const auto authServiceSource = source("service/domains/auth/auth.service.h");
    REQUIRE(authServiceSource.contains("auth_session.service.h"));
    REQUIRE(!authServiceSource.contains("sys_auth_session"));
    REQUIRE(!authServiceSource.contains("createSession"));
    const auto authSessionService = source("service/domains/auth/auth_session.service.h");
    REQUIRE(authSessionService.contains("class AuthSessionService final"));
    REQUIRE(authSessionService.contains("sys_auth_session"));
    REQUIRE(authSessionService.contains("readSessionCookie"));
    const auto authController = source("service/domains/auth/auth.controller.h");
    REQUIRE(authController.contains("authSessionService().refresh"));
    REQUIRE(authController.contains("authSessionService().logout"));
    const auto dnsProviderServiceSource = source("service/domains/provider/dns_provider.service.h");
    REQUIRE(dnsProviderServiceSource.contains("dns_provider_read.service.h"));
    REQUIRE(dnsProviderServiceSource.contains("dnsProviderReadService().list"));
    REQUIRE(dnsProviderServiceSource.contains("dnsProviderReadService().get"));
    REQUIRE(!dnsProviderServiceSource.contains("struct StoredProvider final"));
    REQUIRE(!dnsProviderServiceSource.contains("static StoredProvider parseRow"));
    const auto dnsProviderReadService =
        source("service/domains/provider/dns_provider_read.service.h");
    REQUIRE(dnsProviderReadService.contains("class DnsProviderReadService final"));
    REQUIRE(dnsProviderReadService.contains("struct StoredProvider final"));
    REQUIRE(dnsProviderReadService.contains("static StoredProvider parseRow"));
    const auto certificateProviderServiceSource =
        source("service/domains/provider/certificate_provider.service.h");
    REQUIRE(certificateProviderServiceSource.contains("certificate_provider_read.service.h"));
    REQUIRE(certificateProviderServiceSource.contains("certificateProviderReadService().list"));
    REQUIRE(!certificateProviderServiceSource.contains("static void fill"));
    const auto certificateProviderReadService =
        source("service/domains/provider/certificate_provider_read.service.h");
    REQUIRE(certificateProviderReadService.contains("class CertificateProviderReadService final"));
    REQUIRE(certificateProviderReadService.contains("static void fill"));
    REQUIRE(!acme.contains("struct AcmeSettings final"));
    const auto acmeTypes = source("service/features/certificate/acme_types.h");
    REQUIRE(acmeTypes.contains("struct AcmeSettings final"));
    REQUIRE(acmeTypes.contains("class AcmeError final"));
    REQUIRE(acmeTypes.contains("struct EabCredentials final"));
    const auto certificateProviderConfig = source("service/features/certificate/provider_config.h");
    REQUIRE(certificateProviderConfig.contains("certificate/acme_types.h"));
    REQUIRE(!certificateProviderConfig.contains("struct EabCredentials final"));
    const auto certificateProvider = source("service/features/certificate/provider.h");
    REQUIRE(certificateProvider.contains("certificate/acme_types.h"));
    REQUIRE(!certificateProvider.contains("certificate/acme.h"));
    const auto acmeAccount = source("service/features/certificate/acme_account.h");
    REQUIRE(acmeAccount.contains("ensureAcmeAccount"));
    REQUIRE(acmeAccount.contains("parseCertificateProviderRuntime"));
    REQUIRE(acmeAccount.contains("SELECT revision, runtime::text FROM sys_provider"));
    const auto certificateWorker = source("service/features/certificate/worker.h");
    REQUIRE(certificateWorker.contains("certificate/acme_account.h"));
    REQUIRE(certificateWorker.contains("certificate/maintenance.h"));
    REQUIRE(certificateWorker.contains("certificate/task.h"));
    REQUIRE(certificateWorker.contains("certificate/task_loader.h"));
    REQUIRE(certificateWorker.contains("certificate/work_loader.h"));
    REQUIRE(certificateWorker.contains("certificate/persistence.h"));
    REQUIRE(!certificateWorker.contains("struct CertificateTask final"));
    REQUIRE(!certificateWorker.contains("struct CertificateWork final"));
    REQUIRE(!certificateWorker.contains("inline ruvia::Task<std::optional<CertificateTask>>"));
    REQUIRE(!certificateWorker.contains("inline ruvia::Task<void> reconcile"));
    REQUIRE(!certificateWorker.contains("inline ruvia::Task<CertificateWork>\nloadWork"));
    REQUIRE(!certificateWorker.contains("CertificateMaterialOutput material"));
    REQUIRE(!certificateWorker.contains("inline ruvia::Task<void>\nfail("));
    const auto certificateTask = source("service/features/certificate/task.h");
    REQUIRE(certificateTask.contains("struct CertificateTask final"));
    const auto certificateTaskLoader = source("service/features/certificate/task_loader.h");
    REQUIRE(certificateTaskLoader.contains("inline ruvia::Task<std::optional<CertificateTask>>"));
    REQUIRE(certificateTaskLoader.contains("FOR UPDATE OF marker SKIP"));
    REQUIRE(certificateTaskLoader.contains("LOCKED LIMIT"));
    const auto certificateMaintenance = source("service/features/certificate/maintenance.h");
    REQUIRE(certificateMaintenance.contains("expireCertificates"));
    REQUIRE(certificateMaintenance.contains("reconcileMissingIssuanceMarkers"));
    REQUIRE(certificateMaintenance.contains("scheduleAutoRenewals"));
    REQUIRE(certificateMaintenance.contains("reconcileCertificateMaintenance"));
    REQUIRE(certificateMaintenance.contains("enqueueCertificateRevision"));
    REQUIRE(certificateMaintenance.contains("enqueueCertificateConsumers"));
    const auto certificateWorkLoader = source("service/features/certificate/work_loader.h");
    REQUIRE(certificateWorkLoader.contains("struct CertificateWork final"));
    REQUIRE(certificateWorkLoader.contains("loadWork"));
    const auto certificatePersistence = source("service/features/certificate/persistence.h");
    REQUIRE(certificatePersistence.contains("markIssuanceStarted"));
    REQUIRE(certificatePersistence.contains("persistIssuedCertificate"));
    REQUIRE(certificatePersistence.contains("failCertificateTask"));
    REQUIRE(certificatePersistence.contains("commitAndPublishResultEvent"));
    REQUIRE(certificatePersistence.contains("completeRunningAndRecordEvent"));
    REQUIRE(certificatePersistence.contains("failRunningAndRecordEvent"));
    REQUIRE(!certificateWorker.contains("commitAndPublishResultEvent"));
    REQUIRE(!certificateWorker.contains("eventRecorded"));
    const auto websitePage = source("web/features/websites/index.tsx");
    REQUIRE(websitePage.contains("import { AccessLogSheet } from './access-log-sheet'"));
    REQUIRE(websitePage.contains("import { WebsiteDialog } from './website-dialog'"));
    REQUIRE(websitePage.contains("import { WebsiteDetailSheet } from './website-detail-sheet'"));
    REQUIRE(websitePage.contains("import { originGroupLabel } from './website-display'"));
    REQUIRE(!websitePage.contains("function AccessLogSheet"));
    REQUIRE(!websitePage.contains("function WebsiteDialog"));
    REQUIRE(!websitePage.contains("function WebsiteDetailSheet"));
    REQUIRE(!websitePage.contains("function originGroupLabel"));
    REQUIRE(!websitePage.contains("useFieldArray"));
    const auto websiteTypes = source("web/features/websites/types.ts");
    REQUIRE(websiteTypes.contains("export type WebsiteConfig"));
    REQUIRE(websiteTypes.contains("export type WebsiteDashboard"));
    const auto frontendSharedTypes = source("web/lib/types.ts");
    REQUIRE(!frontendSharedTypes.contains("export type WebsiteConfig"));
    REQUIRE(!frontendSharedTypes.contains("export type WebsiteDashboard"));
    const auto websiteDialog = source("web/features/websites/website-dialog.tsx");
    REQUIRE(websiteDialog.contains("export function WebsiteDialog"));
    REQUIRE(websiteDialog.contains("useFieldArray"));
    REQUIRE(websiteDialog.contains("from './website-form'"));
    REQUIRE(websiteDialog.contains("from './website-basic-tab'"));
    REQUIRE(websiteDialog.contains("from './website-domains-tab'"));
    REQUIRE(websiteDialog.contains("from './website-features-tab'"));
    REQUIRE(websiteDialog.contains("from './website-origins-tab'"));
    REQUIRE(websiteDialog.contains("from './website-routes-tab'"));
    REQUIRE(!websiteDialog.contains("const domainSchema"));
    REQUIRE(!websiteDialog.contains("function defaultConfig"));
    REQUIRE(!websiteDialog.contains("<TabsContent value='basic'"));
    REQUIRE(!websiteDialog.contains("<TabsContent value='domains'"));
    REQUIRE(!websiteDialog.contains("<TabsContent value='features'"));
    REQUIRE(!websiteDialog.contains("<TabsContent value='origins'"));
    REQUIRE(!websiteDialog.contains("<TabsContent value='routes'"));
    const auto websiteDisplay = source("web/features/websites/website-display.ts");
    REQUIRE(websiteDisplay.contains("export function originGroupLabel"));
    const auto websiteForm = source("web/features/websites/website-form.ts");
    REQUIRE(websiteForm.contains("export const websiteFormSchema"));
    REQUIRE(websiteForm.contains("export function defaultWebsiteConfig"));
    REQUIRE(websiteForm.contains("export function parseRouteHeaders"));
    REQUIRE(websiteForm.contains("export function valuesToLines"));
    REQUIRE(websiteForm.contains("export const routeMethods"));
    const auto websiteBasicTab = source("web/features/websites/website-basic-tab.tsx");
    REQUIRE(websiteBasicTab.contains("export function WebsiteBasicTab"));
    REQUIRE(websiteBasicTab.contains("<TabsContent value='basic'"));
    const auto websiteDomainsTab = source("web/features/websites/website-domains-tab.tsx");
    REQUIRE(websiteDomainsTab.contains("export function WebsiteDomainsTab"));
    REQUIRE(websiteDomainsTab.contains("<TabsContent value='domains'"));
    const auto websiteFeaturesTab = source("web/features/websites/website-features-tab.tsx");
    REQUIRE(websiteFeaturesTab.contains("export function WebsiteFeaturesTab"));
    REQUIRE(websiteFeaturesTab.contains("<TabsContent value='features'"));
    const auto websiteOriginsTab = source("web/features/websites/website-origins-tab.tsx");
    REQUIRE(websiteOriginsTab.contains("export function WebsiteOriginsTab"));
    REQUIRE(websiteOriginsTab.contains("<TabsContent value='origins'"));
    const auto websiteRoutesTab = source("web/features/websites/website-routes-tab.tsx");
    REQUIRE(websiteRoutesTab.contains("export function WebsiteRoutesTab"));
    REQUIRE(websiteRoutesTab.contains("<TabsContent value='routes'"));

    const auto dnsZonesPage = source("web/features/dns-zones/index.tsx");
    REQUIRE(dnsZonesPage.contains("import { CreateZoneDialog } from './create-zone-dialog'"));
    REQUIRE(dnsZonesPage.contains("import { RecordsDialog } from './records-dialog'"));
    REQUIRE(dnsZonesPage.contains("import { ZoneDetailSheet } from './zone-detail-sheet'"));
    REQUIRE(!dnsZonesPage.contains("function CreateZoneDialog"));
    REQUIRE(!dnsZonesPage.contains("function RecordsDialog"));
    REQUIRE(!dnsZonesPage.contains("function ZoneDetailSheet"));
    REQUIRE(!dnsZonesPage.contains("useFieldArray"));
    const auto createZoneDialog = source("web/features/dns-zones/create-zone-dialog.tsx");
    REQUIRE(createZoneDialog.contains("export function CreateZoneDialog"));
    REQUIRE(createZoneDialog.contains("createSchema"));
    const auto recordsDialog = source("web/features/dns-zones/records-dialog.tsx");
    REQUIRE(recordsDialog.contains("export function RecordsDialog"));
    REQUIRE(recordsDialog.contains("useFieldArray"));
    const auto zoneDetailSheet = source("web/features/dns-zones/zone-detail-sheet.tsx");
    REQUIRE(zoneDetailSheet.contains("export function ZoneDetailSheet"));
    const auto dnsZoneDisplay = source("web/features/dns-zones/dns-zone-display.ts");
    REQUIRE(dnsZoneDisplay.contains("export function displaySyncStatus"));
    REQUIRE(dnsZoneDisplay.contains("export function hasMeaningfulConflicts"));

    const auto providersPage = source("web/features/providers/index.tsx");
    REQUIRE(providersPage.contains("import { DnsProviderDialog } from './dns-provider-dialog'"));
    REQUIRE(providersPage.contains(
        "import { CertificateProviderDialog } from './certificate-provider-dialog'"));
    REQUIRE(providersPage.contains("import { providerLabel } from './provider-display'"));
    REQUIRE(!providersPage.contains("function DnsProviderDialog"));
    REQUIRE(!providersPage.contains("function CertificateProviderDialog"));
    REQUIRE(!providersPage.contains("function providerLabel"));
    const auto dnsProviderDialog = source("web/features/providers/dns-provider-dialog.tsx");
    REQUIRE(dnsProviderDialog.contains("export function DnsProviderDialog"));
    REQUIRE(dnsProviderDialog.contains("dnsSchema"));
    const auto certificateProviderDialog =
        source("web/features/providers/certificate-provider-dialog.tsx");
    REQUIRE(certificateProviderDialog.contains("export function CertificateProviderDialog"));
    REQUIRE(certificateProviderDialog.contains("certificateSchema"));
    const auto providerDisplay = source("web/features/providers/provider-display.ts");
    REQUIRE(providerDisplay.contains("export function providerLabel"));

    const auto nodesPage = source("web/features/nodes/index.tsx");
    REQUIRE(nodesPage.contains("import { CredentialsDialog } from './credentials-dialog'"));
    REQUIRE(nodesPage.contains("import { NodeDialog, type NodeCredentials } from './node-dialog'"));
    REQUIRE(nodesPage.contains("import { NodeLogSheet } from './node-log-sheet'"));
    REQUIRE(!nodesPage.contains("function NodeDialog"));
    REQUIRE(!nodesPage.contains("function CredentialsDialog"));
    REQUIRE(!nodesPage.contains("function NodeLogSheet"));
    REQUIRE(!nodesPage.contains("useFieldArray"));
    const auto nodeDialog = source("web/features/nodes/node-dialog.tsx");
    REQUIRE(nodeDialog.contains("export function NodeDialog"));
    REQUIRE(nodeDialog.contains("export type NodeCredentials"));
    REQUIRE(nodeDialog.contains("useFieldArray"));
    const auto credentialsDialog = source("web/features/nodes/credentials-dialog.tsx");
    REQUIRE(credentialsDialog.contains("export function CredentialsDialog"));
    const auto nodeLogSheet = source("web/features/nodes/node-log-sheet.tsx");
    REQUIRE(nodeLogSheet.contains("export function NodeLogSheet"));
    REQUIRE(nodeLogSheet.contains("new EventSource"));

    const auto certificatesPage = source("web/features/certificates/index.tsx");
    REQUIRE(certificatesPage.contains("import { CertificateDialog } from './certificate-dialog'"));
    REQUIRE(certificatesPage.contains(
        "import { CertificateDetailSheet } from './certificate-detail-sheet'"));
    REQUIRE(!certificatesPage.contains("function CertificateDialog"));
    REQUIRE(!certificatesPage.contains("function CertificateDetailSheet"));
    const auto certificateDialog = source("web/features/certificates/certificate-dialog.tsx");
    REQUIRE(certificateDialog.contains("export function CertificateDialog"));
    REQUIRE(certificateDialog.contains("createSchema"));
    const auto certificateDetailSheet =
        source("web/features/certificates/certificate-detail-sheet.tsx");
    REQUIRE(certificateDetailSheet.contains("export function CertificateDetailSheet"));
    REQUIRE(certificateDetailSheet.contains("fingerprint_sha256"));

    const auto clustersPage = source("web/features/clusters/index.tsx");
    REQUIRE(clustersPage.contains("import { ClusterDialog } from './cluster-dialog'"));
    REQUIRE(!clustersPage.contains("function ClusterDialog"));
    REQUIRE(!clustersPage.contains("useForm"));
    const auto clusterDialog = source("web/features/clusters/cluster-dialog.tsx");
    REQUIRE(clusterDialog.contains("export function ClusterDialog"));
    REQUIRE(clusterDialog.contains("const schema = z.object"));

    const auto syncRuntime = source("service/features/sync_runtime/state.h");
    REQUIRE(syncRuntime.contains("namespace service::sync_runtime"));
    REQUIRE(syncRuntime.contains("enum class MarkerResourceType"));
    REQUIRE(syncRuntime.contains("enum class MarkerOperation"));
    REQUIRE(syncRuntime.contains("supportsMarkerOperation"));
    REQUIRE(syncRuntime.contains("resourceTypeName(MarkerResourceType resourceType)"));
    REQUIRE(syncRuntime.contains("resourceColumn(MarkerResourceType resourceType)"));
    REQUIRE(!syncRuntime.contains("resourceColumn(std::string_view"));
    REQUIRE(syncRuntime.contains("ON CONFLICT (tenant_id, resource_type, "));
    REQUIRE(syncRuntime.contains("resource_id) DO UPDATE SET"));
    REQUIRE(syncRuntime.contains("GREATEST(sys_sync_task.version, "));
    REQUIRE(syncRuntime.contains("EXCLUDED.version), operation"));
    REQUIRE(syncRuntime.contains("renewRunningLease"));
    REQUIRE(syncRuntime.contains("completeRunning"));
    REQUIRE(syncRuntime.contains("completeRunningAndRecordEvent"));
    REQUIRE(syncRuntime.contains("failRunningAndRecordEvent"));
    REQUIRE(syncRuntime.contains("recordRunningResultEvent"));
    REQUIRE(syncRuntime.contains("struct RunningResultTransition final"));
    REQUIRE(syncRuntime.contains("bool eventRecorded"));
    REQUIRE(syncRuntime.contains("commitAndPublishResultEvent"));
    REQUIRE(syncRuntime.contains("publishRecordedResultEvent"));
    REQUIRE(!syncRuntime.contains("publishResultEvent"));
    REQUIRE(syncRuntime.contains("pruneResultEvents"));
    REQUIRE(syncRuntime.contains("removeRunning"));
    REQUIRE(syncRuntime.contains("recoverStaleRunning"));
    REQUIRE(syncRuntime.contains("makeRunningLease"));
    REQUIRE(!syncRuntime.contains("kMaximumFailures"));

    for (const auto* markerWriterPath : {
             "service/features/provider_verification/queue.h",
             "service/features/dns_sync/queue.h",
             "service/features/certificate/queue.h",
             "service/domains/website/website_command.service.h",
             "service/domains/certificate/certificate.service.h",
         }) {
        const auto markerWriter = source(markerWriterPath);
        REQUIRE(markerWriter.contains("MarkerResourceType::"));
        REQUIRE(markerWriter.contains("MarkerOperation"));
        REQUIRE(!markerWriter.contains("upsertMarker(transaction, tenantId, \""));
        REQUIRE(!markerWriter.contains("removeMarker(transaction, tenantId, \""));
    }

    const auto dnsSyncWorker = source("service/features/dns_sync/worker.h");
    REQUIRE(dnsSyncWorker.contains("dns_sync/failure.h"));
    REQUIRE(dnsSyncWorker.contains("dns_sync/maintenance.h"));
    REQUIRE(dnsSyncWorker.contains("dns_sync/sync_engine.h"));
    REQUIRE(dnsSyncWorker.contains("dns_sync/task_loader.h"));
    REQUIRE(!dnsSyncWorker.contains("dns_sync/reconciliation.h"));
    REQUIRE(!dnsSyncWorker.contains("dns_sync/zone_loader.h"));
    REQUIRE(!dnsSyncWorker.contains("dns_sync/zone_persistence.h"));
    REQUIRE(!dnsSyncWorker.contains("struct ManagedRecord final"));
    REQUIRE(!dnsSyncWorker.contains("inline RemoteMergePlan\nplanRemoteMerge"));
    REQUIRE(!dnsSyncWorker.contains("inline ruvia::Task<bool>\nmergeRemoteRecords"));
    REQUIRE(!dnsSyncWorker.contains("struct TaskFailure final"));
    REQUIRE(!dnsSyncWorker.contains("inline TaskFailure classifyTaskFailure"));
    REQUIRE(!dnsSyncWorker.contains("struct DnsTask final"));
    REQUIRE(!dnsSyncWorker.contains("inline ruvia::Task<std::optional<DnsTask>> claim"));
    REQUIRE(!dnsSyncWorker.contains("inline ruvia::Task<void> reconcileTasks"));
    REQUIRE(!dnsSyncWorker.contains("inline ruvia::Task<void> fail("));
    REQUIRE(!dnsSyncWorker.contains("commitAndPublishResultEvent"));
    REQUIRE(!dnsSyncWorker.contains("eventRecorded"));
    REQUIRE(!dnsSyncWorker.contains("struct ZoneSyncState final"));
    REQUIRE(!dnsSyncWorker.contains("struct RemoteZoneData final"));
    REQUIRE(!dnsSyncWorker.contains("struct DesiredRecordIds final"));
    REQUIRE(!dnsSyncWorker.contains("inline ruvia::Task<ZoneRuntimeDto> reconcileZoneRecords"));
    REQUIRE(!dnsSyncWorker.contains("inline ruvia::Task<std::int64_t> syncZone"));
    REQUIRE(!dnsSyncWorker.contains("inline ruvia::Task<void>\nimportInitialRemoteRecords"));
    REQUIRE(!dnsSyncWorker.contains("inline ruvia::Task<bool> persistRemoteMerge"));
    REQUIRE(!dnsSyncWorker.contains("inline ruvia::Task<void> persistSyncedZone"));
    const auto dnsSyncTask = source("service/features/dns_sync/task.h");
    REQUIRE(dnsSyncTask.contains("struct DnsTask final"));
    const auto dnsSyncTaskLoader = source("service/features/dns_sync/task_loader.h");
    REQUIRE(dnsSyncTaskLoader.contains("inline ruvia::Task<std::optional<DnsTask>> claim"));
    REQUIRE(dnsSyncTaskLoader.contains("FOR UPDATE OF task SKIP LOCKED"));
    const auto dnsMaintenance = source("service/features/dns_sync/maintenance.h");
    REQUIRE(dnsMaintenance.contains("reconcileTasks"));
    REQUIRE(dnsMaintenance.contains("pruneClusterManagedRecords"));
    REQUIRE(dnsMaintenance.contains("enqueueZoneRevision"));
    const auto dnsSyncEngine = source("service/features/dns_sync/sync_engine.h");
    REQUIRE(dnsSyncEngine.contains("mergeRemoteRecords"));
    REQUIRE(dnsSyncEngine.contains("struct DesiredRecordIds final"));
    REQUIRE(dnsSyncEngine.contains("deleteObsoleteRemoteRecords"));
    REQUIRE(dnsSyncEngine.contains("reconcileZoneRecords"));
    REQUIRE(dnsSyncEngine.contains("inline ruvia::Task<std::int64_t> syncZone"));
    const auto dnsZoneLoader = source("service/features/dns_sync/zone_loader.h");
    REQUIRE(!dnsZoneLoader.contains("struct DnsTask final"));
    REQUIRE(dnsZoneLoader.contains("struct ZoneSyncState final"));
    REQUIRE(dnsZoneLoader.contains("struct RemoteZoneData final"));
    REQUIRE(dnsZoneLoader.contains("loadZoneSyncState"));
    REQUIRE(dnsZoneLoader.contains("loadRemoteZoneData"));
    const auto dnsZonePersistence = source("service/features/dns_sync/zone_persistence.h");
    REQUIRE(dnsZonePersistence.contains("importInitialRemoteRecords"));
    REQUIRE(dnsZonePersistence.contains("storeConflicts"));
    REQUIRE(dnsZonePersistence.contains("persistRemoteMerge"));
    REQUIRE(dnsZonePersistence.contains("persistSyncedZone"));
    REQUIRE(dnsZonePersistence.contains("failDnsTask"));
    REQUIRE(dnsZonePersistence.contains("failRunningAndRecordEvent"));
    REQUIRE(dnsZonePersistence.contains("commitAndPublishResultEvent"));
    const auto dnsReconciliation = source("service/features/dns_sync/reconciliation.h");
    REQUIRE(dnsReconciliation.contains("struct ManagedRecord final"));
    REQUIRE(dnsReconciliation.contains("inline RemoteMergePlan\nplanRemoteMerge"));
    REQUIRE(dnsReconciliation.contains("kMaxReconciliationRecords"));
    const auto dnsFailure = source("service/features/dns_sync/failure.h");
    REQUIRE(dnsFailure.contains("struct TaskFailure final"));
    REQUIRE(dnsFailure.contains("inline TaskFailure classifyTaskFailure"));
    REQUIRE(dnsFailure.contains("sync_runtime/error.h"));
    REQUIRE(!dnsFailure.contains("inline std::string boundedError"));

    const auto syncRuntimeError = source("service/features/sync_runtime/error.h");
    REQUIRE(syncRuntimeError.contains("kMaxErrorMessageLength{1000}"));
    REQUIRE(syncRuntimeError.contains("inline std::string boundedError"));
    const auto websiteDispatchWorker = source("service/features/website_dispatch/worker.h");
    REQUIRE(websiteDispatchWorker.contains("website_dispatch/persistence.h"));
    REQUIRE(websiteDispatchWorker.contains("website_dispatch/task.h"));
    REQUIRE(!websiteDispatchWorker.contains("struct WebsiteMarker final"));
    REQUIRE(!websiteDispatchWorker.contains("inline ruvia::Task<void> reconcileMarkers"));
    REQUIRE(!websiteDispatchWorker.contains("inline ruvia::Task<void> fail("));
    REQUIRE(!websiteDispatchWorker.contains("commitAndPublishResultEvent"));
    REQUIRE(!websiteDispatchWorker.contains("eventRecorded"));
    const auto websiteDispatchTask = source("service/features/website_dispatch/task.h");
    REQUIRE(websiteDispatchTask.contains("struct WebsiteMarker final"));
    const auto websiteDispatchPersistence =
        source("service/features/website_dispatch/persistence.h");
    REQUIRE(websiteDispatchPersistence.contains("reconcileMarkers"));
    REQUIRE(
        websiteDispatchPersistence.contains("inline ruvia::Task<std::optional<WebsiteMarker>>"));
    REQUIRE(websiteDispatchPersistence.contains("failWebsiteMarker"));
    REQUIRE(websiteDispatchPersistence.contains("failRunningAndRecordEvent"));
    REQUIRE(websiteDispatchPersistence.contains("commitAndPublishResultEvent"));
    const auto providerVerificationWorker =
        source("service/features/provider_verification/worker.h");
    REQUIRE(providerVerificationWorker.contains("provider_verification/failure.h"));
    REQUIRE(providerVerificationWorker.contains("provider_verification/persistence.h"));
    REQUIRE(providerVerificationWorker.contains("provider_verification/task.h"));
    REQUIRE(providerVerificationWorker.contains("provider_verification/task_loader.h"));
    REQUIRE(providerVerificationWorker.contains("provider_verification/verification.h"));
    REQUIRE(!providerVerificationWorker.contains("class VerificationError final"));
    REQUIRE(!providerVerificationWorker.contains("inline TaskFailure classifyTaskFailure"));
    REQUIRE(!providerVerificationWorker.contains("struct VerificationTask final"));
    REQUIRE(!providerVerificationWorker.contains("struct VerificationResult final"));
    REQUIRE(!providerVerificationWorker.contains(
        "inline ruvia::Task<std::optional<VerificationTask>>"));
    REQUIRE(!providerVerificationWorker.contains("parseRuntimeEab"));
    REQUIRE(!providerVerificationWorker.contains("fetchZeroSslEab"));
    REQUIRE(!providerVerificationWorker.contains("inline ruvia::Task<void> completeMarker"));
    REQUIRE(!providerVerificationWorker.contains("inline ruvia::Task<void> fail("));
    REQUIRE(!providerVerificationWorker.contains("commitAndPublishResultEvent"));
    REQUIRE(!providerVerificationWorker.contains("eventRecorded"));
    const auto providerVerificationTask = source("service/features/provider_verification/task.h");
    REQUIRE(providerVerificationTask.contains("struct VerificationTask final"));
    const auto providerVerificationTaskLoader =
        source("service/features/provider_verification/task_loader.h");
    REQUIRE(providerVerificationTaskLoader.contains(
        "inline ruvia::Task<std::optional<VerificationTask>>"));
    REQUIRE(providerVerificationTaskLoader.contains("FOR UPDATE OF marker SKIP"));
    REQUIRE(providerVerificationTaskLoader.contains("LOCKED LIMIT 1"));
    const auto providerVerification =
        source("service/features/provider_verification/verification.h");
    REQUIRE(providerVerification.contains("struct VerificationResult final"));
    REQUIRE(providerVerification.contains("parseRuntimeEab"));
    REQUIRE(providerVerification.contains("fetchZeroSslEab"));
    REQUIRE(providerVerification.contains("verifyDns"));
    REQUIRE(providerVerification.contains("verifyCertificate"));
    const auto providerVerificationPersistence =
        source("service/features/provider_verification/persistence.h");
    REQUIRE(providerVerificationPersistence.contains("inline ruvia::Task<void> reconcile"));
    REQUIRE(providerVerificationPersistence.contains("loadCurrentRuntime"));
    REQUIRE(providerVerificationPersistence.contains("completeVerification"));
    REQUIRE(providerVerificationPersistence.contains("failVerification"));
    REQUIRE(providerVerificationPersistence.contains("completeRunningAndRecordEvent"));
    REQUIRE(providerVerificationPersistence.contains("failRunningAndRecordEvent"));
    REQUIRE(providerVerificationPersistence.contains("commitAndPublishResultEvent"));
    const auto providerVerificationFailure =
        source("service/features/provider_verification/failure.h");
    REQUIRE(providerVerificationFailure.contains("class VerificationError final"));
    REQUIRE(providerVerificationFailure.contains("inline TaskFailure classifyTaskFailure"));
    REQUIRE(service::sync_runtime::boundedError(
                std::string(service::sync_runtime::kMaxErrorMessageLength + 1, 'x'))
                .size() == service::sync_runtime::kMaxErrorMessageLength);
    const auto invalidProviderFailure = service::provider_verification::detail::classifyTaskFailure(
        std::make_exception_ptr(std::invalid_argument("invalid provider config")));
    REQUIRE(invalidProviderFailure.permanent);
    const auto retryableProviderFailure =
        service::provider_verification::detail::classifyTaskFailure(
            std::make_exception_ptr(service::certificate_issuance::CertificateProviderClientError(
                "upstream unavailable", true)));
    REQUIRE(!retryableProviderFailure.permanent);
    for (const auto workerPath : {
             "service/features/provider_verification/worker.h",
             "service/features/certificate/worker.h",
             "service/features/website_dispatch/worker.h",
         }) {
        const auto worker = source(workerPath);
        REQUIRE(worker.contains("sync_runtime/error.h"));
        REQUIRE(!worker.contains("inline std::string boundedError"));
        REQUIRE(worker.contains("service::sync_runtime::boundedError"));
    }

    const auto markerWorkerLoop = source("service/features/background/marker_worker_loop.h");
    REQUIRE(markerWorkerLoop.contains("runMarkerWorkerLoop"));
    REQUIRE(markerWorkerLoop.contains("nextLeaseRecovery"));
    REQUIRE(markerWorkerLoop.contains("nextReconciliation"));
    REQUIRE(markerWorkerLoop.contains("nextEventPrune"));
    for (const auto workerPath : {
             "service/features/provider_verification/worker.h",
             "service/features/dns_sync/worker.h",
             "service/features/certificate/worker.h",
             "service/features/website_dispatch/worker.h",
         }) {
        const auto worker = source(workerPath);
        REQUIRE(worker.contains("background/marker_worker_loop.h"));
        REQUIRE(worker.contains("runMarkerWorkerLoop"));
    }

    const auto logNotifications = source("service/features/log_ingest/notifications.h");
    REQUIRE(logNotifications.contains("flexedge:log-notifications:v2"));
    REQUIRE(logNotifications.contains("enum class LogResourceType"));
    REQUIRE(logNotifications.contains("resource_type"));
    REQUIRE(logNotifications.contains("resource_id"));
    REQUIRE(logNotifications.contains("struct ReadBatch final"));
    REQUIRE(!logNotifications.contains("publishAccess"));
    REQUIRE(!logNotifications.contains("publishNode"));
    REQUIRE(!logNotifications.contains("websiteId"));
    REQUIRE(!logNotifications.contains("nodeId"));
    const auto logFanout = source("service/features/log_ingest/fanout.h");
    REQUIRE(logFanout.contains("LogResourceType resourceType"));
    REQUIRE(logFanout.contains("subscribeTopic"));
    REQUIRE(logFanout.contains("batch.notifications"));
    REQUIRE(!logFanout.contains("subscribeAccess"));
    REQUIRE(!logFanout.contains("subscribeNode"));
    const auto logSseTail = source("service/features/log_ingest/sse_tail.h");
    REQUIRE(logSseTail.contains("streamSseTail"));
    REQUIRE(logSseTail.contains("tailResponseCursor"));
    REQUIRE(logSseTail.contains("advanceTailCursor"));
    REQUIRE(logSseTail.contains("sseClientDisconnected"));
    const auto nodeController = source("service/domains/node/node.controller.h");
    REQUIRE(nodeController.contains("log_ingest/sse_tail.h"));
    REQUIRE(nodeController.contains("streamSseTail"));
    REQUIRE(nodeController.contains("node_runtime/fanout.h"));
    REQUIRE(nodeController.contains("runtimeStreamBody"));
    REQUIRE(nodeController.contains("node_runtime::fanout::hub().subscribe"));
    REQUIRE(nodeController.contains("subscription.receiveFor("));
    REQUIRE(nodeController.contains("event = \"node-state\""));
    REQUIRE(nodeController.contains("node_command.service.h"));
    REQUIRE(nodeController.contains("node_read.service.h"));
    REQUIRE(!nodeController.contains("node.service.h"));
    REQUIRE(nodeController.contains("nodeCommandService().create"));
    REQUIRE(nodeController.contains("nodeReadService().list"));
    REQUIRE(!nodeController.contains("advanceCursor("));
    const auto websiteController = source("service/domains/website/website.controller.h");
    REQUIRE(websiteController.contains("website_command.service.h"));
    REQUIRE(websiteController.contains("website_read.service.h"));
    REQUIRE(websiteController.contains("website_access_log.service.h"));
    REQUIRE(websiteController.contains("website_dashboard.service.h"));
    REQUIRE(!websiteController.contains("website.service.h"));
    REQUIRE(websiteController.contains("websiteCommandService().create"));
    REQUIRE(websiteController.contains("websiteReadService().list"));
    REQUIRE(websiteController.contains("log_ingest/sse_tail.h"));
    REQUIRE(websiteController.contains("streamSseTail"));
    REQUIRE(!websiteController.contains("receiveFor("));
    REQUIRE(!websiteController.contains("advanceCursor("));
    const auto nodeRuntimeFanout = source("service/features/node_runtime/fanout.h");
    REQUIRE(nodeRuntimeFanout.contains("kSubscriberSignalCapacity{1}"));
    REQUIRE(nodeRuntimeFanout.contains("class Hub final"));
    REQUIRE(nodeRuntimeFanout.contains("Subscription"));
    REQUIRE(nodeRuntimeFanout.contains("subscribe"));
    REQUIRE(nodeRuntimeFanout.contains("publish"));

    REQUIRE(!std::filesystem::exists(sourceRoot / "service/domains/task/task.controller.h"));
    REQUIRE(!std::filesystem::exists(sourceRoot / "service/domains/task/task.service.h"));
    REQUIRE(!std::filesystem::exists(sourceRoot / "service/domains/task/task.types.h"));
    REQUIRE(!std::filesystem::exists(sourceRoot / "web/features/tasks/index.tsx"));
    REQUIRE(!std::filesystem::exists(sourceRoot / "web/routes/_authenticated/tasks.tsx"));
    const auto syncEventService = source("service/domains/sync_event/sync_event.service.h");
    REQUIRE(syncEventService.contains("SyncEventPageDataDto"));
    REQUIRE(syncEventService.contains("FROM sys_sync_event"));
    REQUIRE(syncEventService.contains("ORDER BY id ASC"));
    const auto syncRuntimeState = source("service/features/sync_runtime/state.h");
    REQUIRE(syncRuntimeState.contains("$4::varchar(16)"));
    const auto syncEventTypes = source("service/domains/sync_event/sync_event.types.h");
    REQUIRE(syncEventTypes.contains("SyncEventDto"));
    REQUIRE(syncEventTypes.contains("has_more"));
    const auto syncEventController = source("service/domains/sync_event/sync_event.controller.h");
    REQUIRE(syncEventController.contains("/api/sync-events"));
    REQUIRE(syncEventController.contains("after 必须是非负事件游标"));
    REQUIRE(!std::filesystem::exists(sourceRoot / "web/components/task-completion-monitor.tsx"));
    const auto syncEventMonitor = source("web/components/sync-event-monitor.tsx");
    REQUIRE(syncEventMonitor.contains("/sync-events/"));
    REQUIRE(syncEventMonitor.contains("queryKeysForSyncEvents"));
    const auto syncEvents = source("web/lib/sync-events.ts");
    REQUIRE(syncEvents.contains("syncEventRefreshKeys"));
    REQUIRE(syncEvents.contains("queryKeys.providers"));
    REQUIRE(syncEvents.contains("queryKeys.dnsZones"));
    REQUIRE(syncEvents.contains("queryKeys.certificates"));
    REQUIRE(syncEvents.contains("queryKeys.websites"));
    REQUIRE(!syncEvents.contains("[['tasks']"));
    REQUIRE(syncEvents.contains("provider:"));
    REQUIRE(syncEvents.contains("dns_zone:"));
    REQUIRE(syncEvents.contains("certificate:"));
    REQUIRE(syncEvents.contains("website:"));
    const auto queryKeys = source("web/lib/query-keys.ts");
    REQUIRE(queryKeys.contains("syncEvents: ['sync-event-monitor']"));
    for (const auto* featurePath :
         {"web/features/certificates/index.tsx", "web/features/clusters/index.tsx",
          "web/features/dns-zones/index.tsx", "web/features/nodes/index.tsx",
          "web/features/providers/index.tsx", "web/features/websites/index.tsx"}) {
        REQUIRE(source(featurePath).contains("queryKeys"));
    }

    const auto nodeDispatch = source("service/features/node_dispatch/queue.h");
    REQUIRE(nodeDispatch.contains("publishClusterRelease"));
    REQUIRE(nodeDispatch.contains("sys_node_release_target"));
    REQUIRE(!nodeDispatch.contains("sys_task"));
    REQUIRE(!nodeDispatch.contains("parent_task_id"));
    REQUIRE(!std::filesystem::exists(sourceRoot / "service/domains/node/node.service.h"));
    const auto nodeCommand = source("service/domains/node/node_command.service.h");
    REQUIRE(nodeCommand.contains("class NodeCommandService final"));
    REQUIRE(nodeCommand.contains("service::utils::randomToken().substr(0, 32)"));
    REQUIRE(!nodeCommand.contains("gen_random_bytes"));
    REQUIRE(nodeCommand.contains("node.mapper.h"));
    REQUIRE(nodeCommand.contains("cluster_dns::reconcileCluster"));
    REQUIRE(nodeCommand.contains("replaceEndpointClaims"));
    REQUIRE(!nodeCommand.contains("node_read.service.h"));
    REQUIRE(!nodeCommand.contains("static void fillNode"));
    REQUIRE(!nodeCommand.contains("static NodeRuntimeDto toRuntime"));
    REQUIRE(!nodeCommand.contains("static std::string serializeConfig"));
    const auto nodeResponseMapper = source("service/domains/node/node.mapper.h");
    REQUIRE(nodeResponseMapper.contains("inline void fillNode"));
    REQUIRE(nodeResponseMapper.contains("inline NodeRuntimeDto nodeRuntimeDto"));
    REQUIRE(nodeResponseMapper.contains("inline std::string serializeNodeConfig"));
    const auto nodeReadService = source("service/domains/node/node_read.service.h");
    REQUIRE(nodeReadService.contains("class NodeReadService final"));
    REQUIRE(nodeReadService.contains("SELECT node.id, node.cluster_id"));
    REQUIRE(nodeReadService.contains("node_secret_envelope"));
    REQUIRE(nodeReadService.contains("FROM sys_node_log entry"));
    REQUIRE(!agentCommand.contains("reconcileReleaseTask"));
    REQUIRE(!agentCommand.contains("sys_task"));
    const auto server = source("service/server.cpp");
    REQUIRE(!server.contains("node-dispatch"));
    REQUIRE(server.contains("website-dispatch"));

    REQUIRE(!dnsSyncWorker.contains("sys_sync_task"));
    REQUIRE(dnsSyncWorker.contains("recoverStaleRunning"));
    REQUIRE(!dnsSyncWorker.contains("publishResultEvent"));
    REQUIRE(!dnsSyncWorker.contains("sys_task"));
    REQUIRE(!dnsSyncWorker.contains("task_runtime"));
    REQUIRE(!dnsSyncWorker.contains("spec_snapshot"));
    REQUIRE(!dnsSyncWorker.contains("RunningMarkerLease lease"));
    const auto dnsSnapshot = source("service/features/dns_sync/snapshot.h");
    REQUIRE(dnsSnapshot.contains("challenge_records"));
    const auto dnsChallenge = source("service/features/certificate/dns_challenge.h");
    REQUIRE(dnsChallenge.contains("RunningMarkerLease"));
    REQUIRE(!dnsChallenge.contains("RunningTaskLease"));
    const auto websiteRuntime = source("service/features/website_dns/runtime.h");
    REQUIRE(websiteRuntime.contains("sync_runtime::completeRunning"));
    REQUIRE(websiteRuntime.contains("commitAndPublishResultEvent"));
    REQUIRE(!websiteRuntime.contains("publishResultEvent"));
    REQUIRE(!websiteRuntime.contains("eventRecorded"));
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
