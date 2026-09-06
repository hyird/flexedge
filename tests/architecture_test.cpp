#include <algorithm>
#include <cctype>
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

    REQUIRE(service::config::kSchemaMigrations.size() == 1);
    REQUIRE(service::config::kSchemaMigrations.front().id() == "0001");
    const auto& schemaBaseline = service::config::kSchemaMigrations.front().sql();
    REQUIRE(!schemaBaseline.contains("sys_task"));
    REQUIRE(!schemaBaseline.contains("node_key_"));
    REQUIRE(!schemaBaseline.contains("device_key_"));
    REQUIRE(!schemaBaseline.contains("DROP TABLE"));
    REQUIRE(!schemaBaseline.contains("UPDATE public."));
    REQUIRE(!schemaBaseline.contains("migrate_data"));
    REQUIRE(schemaBaseline.contains("node_secret_envelope text"));
    REQUIRE(schemaBaseline.contains("schema_version integer NOT NULL"));
    REQUIRE(schemaBaseline.contains("CREATE TABLE public.sys_sync_task"));
    REQUIRE(schemaBaseline.contains("CREATE TABLE public.sys_sync_event"));
    REQUIRE(schemaBaseline.contains("uk_sync_task_resource"));
    REQUIRE(schemaBaseline.contains("idx_sync_event_tenant_id"));
    REQUIRE(schemaBaseline.contains("version bigint NOT NULL"));
    REQUIRE(schemaBaseline.contains("processed_version bigint DEFAULT 0 NOT NULL"));
    REQUIRE(schemaBaseline.contains("lease_until timestamptz"));
    REQUIRE(schemaBaseline.contains(
        "num_nonnulls(provider_id, dns_zone_id, certificate_id, website_id) = 1"));
    REQUIRE(schemaBaseline.contains("request_bytes bigint DEFAULT 0 NOT NULL"));
    REQUIRE(schemaBaseline.contains("idx_website_access_log_website_ingested"));
    REQUIRE(schemaBaseline.contains("idx_node_log_node_ingested"));

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
    REQUIRE(syncRuntime.contains("pruneResultEvents"));
    REQUIRE(syncRuntime.contains("removeRunning"));
    REQUIRE(syncRuntime.contains("recoverStaleRunning"));
    REQUIRE(syncRuntime.contains("makeRunningLease"));
    REQUIRE(!syncRuntime.contains("kMaximumFailures"));

    for (const auto* markerWriterPath : {
             "service/features/provider_verification/queue.h",
             "service/features/dns_sync/queue.h",
             "service/features/certificate/queue.h",
             "service/domains/website/website.service.h",
             "service/domains/certificate/certificate.service.h",
         }) {
        const auto markerWriter = source(markerWriterPath);
        REQUIRE(markerWriter.contains("MarkerResourceType::"));
        REQUIRE(markerWriter.contains("MarkerOperation"));
        REQUIRE(!markerWriter.contains("upsertMarker(transaction, tenantId, \""));
        REQUIRE(!markerWriter.contains("removeMarker(transaction, tenantId, \""));
    }

    const auto dnsSyncWorker = source("service/features/dns_sync/worker.h");
    REQUIRE(dnsSyncWorker.contains("dns_sync/reconciliation.h"));
    REQUIRE(!dnsSyncWorker.contains("struct ManagedRecord final"));
    REQUIRE(!dnsSyncWorker.contains("inline RemoteMergePlan\nplanRemoteMerge"));
    const auto dnsReconciliation = source("service/features/dns_sync/reconciliation.h");
    REQUIRE(dnsReconciliation.contains("struct ManagedRecord final"));
    REQUIRE(dnsReconciliation.contains("inline RemoteMergePlan\nplanRemoteMerge"));
    REQUIRE(dnsReconciliation.contains("kMaxReconciliationRecords"));

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
    for (const auto* logControllerPath : {
             "service/domains/node/node.controller.h",
             "service/domains/website/website.controller.h",
         }) {
        const auto controller = source(logControllerPath);
        REQUIRE(controller.contains("log_ingest/sse_tail.h"));
        REQUIRE(controller.contains("streamSseTail"));
        REQUIRE(!controller.contains("receiveFor("));
        REQUIRE(!controller.contains("advanceCursor("));
    }

    REQUIRE(!std::filesystem::exists(sourceRoot / "service/domains/task/task.controller.h"));
    REQUIRE(!std::filesystem::exists(sourceRoot / "service/domains/task/task.service.h"));
    REQUIRE(!std::filesystem::exists(sourceRoot / "service/domains/task/task.types.h"));
    REQUIRE(!std::filesystem::exists(sourceRoot / "web/features/tasks/index.tsx"));
    REQUIRE(!std::filesystem::exists(sourceRoot / "web/routes/_authenticated/tasks.tsx"));
    const auto syncEventService = source("service/domains/sync_event/sync_event.service.h");
    REQUIRE(syncEventService.contains("SyncEventPageDataDto"));
    REQUIRE(syncEventService.contains("FROM sys_sync_event"));
    REQUIRE(syncEventService.contains("ORDER BY id ASC"));
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
    const auto nodeService = source("service/domains/node/node.service.h");
    REQUIRE(nodeService.contains("service::utils::randomToken().substr(0, 32)"));
    REQUIRE(!nodeService.contains("gen_random_bytes"));
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
        REQUIRE(worker.contains("publishResultEvent"));
        REQUIRE(worker.contains("eventRecorded"));
        REQUIRE(!worker.contains("sys_task"));
        REQUIRE(!worker.contains("task_runtime"));
        REQUIRE(!worker.contains("spec_snapshot"));
        REQUIRE(!worker.contains("RunningMarkerLease lease"));
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
