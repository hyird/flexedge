// Read projections over the real schema: the tenant traffic aggregate, the
// overview node counts, node/cluster detail reads and the list filters that
// share their definitions with the overview counts. The schema is applied and
// populated inside one transaction that is rolled back.
#include <ruvia/web/db/db_transaction.h>
#include <ruvia/web/error.h>
#include <ruvia/web/model_json.h>

#include <cstdint>
#include <cstdlib>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "service/config/schema.h"
#include "service/domains/certificate/certificate_filter.h"
#include "service/domains/certificate/certificate_read_service.h"
#include "service/domains/cluster/cluster_read_service.h"
#include "service/domains/dns_zone/dns_zone_filter.h"
#include "service/domains/dns_zone/dns_zone_read_service.h"
#include "service/domains/node/node_read_service.h"
#include "service/domains/overview/overview_service.h"
#include "service/domains/overview/overview_traffic_service.h"
#include "service/domains/website/website_dashboard_service.h"
#include "service/domains/website/website_filter.h"
#include "service/domains/website/website_read_service.h"
#include "support/postgres_test.h"

#define REQUIRE(condition)                                                             \
    do {                                                                               \
        if (!(condition))                                                              \
            throw std::runtime_error("read projection invariant failed: " #condition); \
    } while (false)

namespace {

// Services read through `c.db()` and allocate DTOs from `c.pool()`. Some keep
// a copy of the handle, so the transaction is exposed through a copyable view.
struct transaction_db final {
    ruvia::db_transaction* transaction_;
    template <typename... args_type>
    auto query(args_type&&... args) const {
        return transaction_->query(std::forward<args_type>(args)...);
    }
};

struct read_context final {
    ruvia::db_transaction* transaction_;
    std::pmr::memory_resource* resource_;
    transaction_db db() const {
        return {transaction_};
    }
    std::pmr::memory_resource* pool() const {
        return resource_;
    }
};

constexpr std::string_view tenant_a{"00000000-0000-4000-8000-00000000000a"};
constexpr std::string_view tenant_b{"00000000-0000-4000-8000-00000000000b"};
constexpr std::string_view tenant_c{"00000000-0000-4000-8000-00000000000c"};
constexpr std::string_view node_online{"00000000-0000-4000-8000-0000000000a1"};
constexpr std::string_view cluster_a{"00000000-0000-4000-8000-0000000000c1"};
constexpr std::string_view cert_expiring{"00000000-0000-4000-8000-0000000000e1"};
constexpr std::string_view cert_later{"00000000-0000-4000-8000-0000000000e2"};
constexpr std::string_view website1{"00000000-0000-4000-8000-000000000101"};
constexpr std::string_view website10{"00000000-0000-4000-8000-000000000110"};
constexpr std::string_view website_deleted{"00000000-0000-4000-8000-000000000111"};
constexpr std::string_view unknown_id{"00000000-0000-4000-8000-0000000000ff"};

std::string website_config(int domain, std::string_view hostname,
    std::optional<std::string_view> name, bool access_log) {
    std::string domain_id = std::to_string(domain);
    domain_id.insert(0, 12 - domain_id.size(), '0');
    std::string config = "{";
    if (name) {
        config += "\"name\":\"" + std::string(*name) + "\",";
    }
    config += R"("domains":[{"id":"11111111-1111-4111-8111-)" + domain_id + R"(","hostname":")" +
              std::string(hostname) + R"(","dns_mode":"managed"}],
    "origins":[{"id":"22222222-2222-4222-8222-222222222222","group":"main","protocol":"http","host":"127.0.0.1","port":80,"role":"primary","weight":1,"status":"enabled"}],
    "default_origin_group":"main","origin_host_header":"$host",
    "origin_connect_timeout_seconds":10,"origin_read_timeout_seconds":10,
    "pass_client_ip":false,"health_check_enabled":false,"health_check_path":"/health",
    "health_check_interval_seconds":30,"health_check_timeout_seconds":5,
    "health_check_expected_status":200,"healthy_threshold":2,"unhealthy_threshold":2,
    "access_log_enabled":)" +
              std::string(access_log ? "true" : "false") +
              R"(,"access_log_request_headers":false,"access_log_request_body":false,
    "access_log_response_headers":false,"access_log_query_params":false,"access_log_cookies":false,
    "access_log_referer":false,"access_log_user_agent":false,"access_log_status_code_ranges":["2xx"],
    "access_log_client_abort":false,"https_enabled":false,"certificate_ids":[],
    "minimum_tls_version":"1.2","force_https":false,"hsts_enabled":false,
    "response_compression_enabled":false,"response_compression_min_bytes":256,
    "response_compression_max_bytes":0,"response_compression_algorithms":["gzip"],
    "response_compression_mime_types":["text/html"],"response_compression_extensions":[".html"],
    "response_compression_excluded_extensions":[],"route_rules":[]})";
    return config;
}

ruvia::task<void> apply_schema(ruvia::db_transaction& tx) {
    // Concurrent runs would race on CREATE EXTENSION; serialize them.
    (void)co_await tx.query("SELECT pg_advisory_xact_lock(7419021)");
    for (const auto& migration : service::config::schema_migrations) {
        (void)co_await tx.execute(migration.sql());
    }
}

ruvia::task<void> insert_fixture(ruvia::db_transaction& tx) {
    const auto exec = [&](std::string sql) -> ruvia::task<void> {
        (void)co_await tx.execute(sql);
    };
    co_await exec(
        "INSERT INTO sys_tenant (id, name, slug) VALUES "
        "('00000000-0000-4000-8000-00000000000a', 'A', 'qa-a'), "
        "('00000000-0000-4000-8000-00000000000b', 'B', 'qa-b'), "
        "('00000000-0000-4000-8000-00000000000c', 'C', 'qa-c')");
    // Per tenant: one DNS provider, one certificate provider, one zone and cluster.
    for (const auto suffix : {std::string_view{"a"}, std::string_view{"b"}}) {
        const std::string s(suffix);
        const auto tenant = "00000000-0000-4000-8000-00000000000" + s;
        co_await exec(
            "INSERT INTO sys_provider (id, tenant_id, kind, provider, name, account_id, "
            "status) VALUES ('00000000-0000-4000-8000-0000000000d" +
            s + "', '" +
            tenant + "', 'dns', 'cloudflare', 'qa-" + s +
            "', 'acct', 'verified'), "
            "('00000000-0000-4000-8000-0000000000f" +
            s + "', '" + tenant +
            "', 'certificate', 'letsencrypt', NULL, NULL, 'verified')");
    }
    const std::string runtime =
        R"('{"records_imported":false,"lines":[],"record_states":[],"conflicts":[]}')";
    co_await exec(
        "INSERT INTO sys_dns_zone (id, tenant_id, provider_id, domain, sync_status, revision, "
        "desired_revision, synced_revision, runtime) VALUES "
        "('00000000-0000-4000-8000-0000000000b1', '" +
        std::string(tenant_a) +
        "', '00000000-0000-4000-8000-0000000000da', 'a.example', 'synced', 1, 1, 1, " + runtime +
        "), "
        "('00000000-0000-4000-8000-0000000000b2', '" +
        std::string(tenant_a) +
        "', '00000000-0000-4000-8000-0000000000da', 'b.example', 'failed', 1, 1, 0, " + runtime +
        "), "
        "('00000000-0000-4000-8000-0000000000b3', '" +
        std::string(tenant_a) +
        "', '00000000-0000-4000-8000-0000000000da', 'c.example', 'conflict', 1, 1, 0, " + runtime +
        "), "
        "('00000000-0000-4000-8000-0000000000b4', '" +
        std::string(tenant_a) +
        "', '00000000-0000-4000-8000-0000000000da', 'd.example', 'pending', 1, 1, 0, " + runtime +
        "), "
        "('00000000-0000-4000-8000-0000000000bb', '" +
        std::string(tenant_b) +
        "', '00000000-0000-4000-8000-0000000000db', 'e.example', 'failed', 1, 1, 0, " + runtime + ")");
    co_await exec(
        "INSERT INTO sys_cluster (id, tenant_id, dns_zone_id, name, hostname_prefix) VALUES "
        "('" +
        std::string(cluster_a) + "', '" + std::string(tenant_a) +
        "', '00000000-0000-4000-8000-0000000000b1', 'edge-a', 'edge'), "
        "('00000000-0000-4000-8000-0000000000cb', '" +
        std::string(tenant_b) +
        "', '00000000-0000-4000-8000-0000000000bb', 'edge-b', 'edge')");
    const auto node = [](std::string_view id, std::string_view tenant, std::string_view cluster,
                          std::string_view name, std::string_view status,
                          std::string_view registration, std::string_view heartbeat,
                          std::string_view deleted) {
        return "('" + std::string(id) + "', '" + std::string(tenant) + "', '" + std::string(cluster) +
               "', '" + std::string(name) + "', '" + std::string(status) + "', '" +
               std::string(registration) + "', " +
               (registration == "registered" ? "NOW()" : "NULL") + ", " + std::string(heartbeat) +
               ", " + std::string(deleted) + ", '" + std::string(32, name.back()) + "', '" +
               std::string(64, name.back()) + "', 'envelope')";
    };
    co_await exec(
        "INSERT INTO sys_node (id, tenant_id, cluster_id, name, status, registration_status, "
        "registered_at, last_heartbeat_at, deleted_at, agent_id, node_secret_hash, "
        "node_secret_envelope) VALUES " +
        node(node_online, tenant_a, cluster_a, "node-1", "enabled", "registered", "NOW()", "NULL") +
        ", " +
        node("00000000-0000-4000-8000-0000000000a2", tenant_a, cluster_a, "node-2", "disabled",
            "registered", "NOW()", "NULL") +
        ", " +
        node("00000000-0000-4000-8000-0000000000a3", tenant_a, cluster_a, "node-3", "enabled",
            "pending", "NULL", "NULL") +
        ", " +
        node("00000000-0000-4000-8000-0000000000a4", tenant_a, cluster_a, "node-4", "enabled",
            "registered", "NOW() - INTERVAL '10 minutes'", "NULL") +
        ", " +
        node("00000000-0000-4000-8000-0000000000a5", tenant_a, cluster_a, "node-5", "enabled",
            "registered", "NOW()", "NOW()") +
        ", " +
        node("00000000-0000-4000-8000-0000000000b9", tenant_b, "00000000-0000-4000-8000-0000000000cb",
            "node-b", "enabled", "registered", "NOW()", "NULL"));
    // Certificates: expiring, later, valid with error, failed, expired, pending,
    // and a deleted expiring one; tenant B holds one expiring certificate.
    const auto certificate = [](std::string_view id, std::string_view tenant, std::string_view status,
                                 std::string_view expires, std::string_view error,
                                 std::string_view deleted) {
        const std::string provider =
            tenant == tenant_a ? "00000000-0000-4000-8000-0000000000fa" : "00000000-0000-4000-8000-0000000000fb";
        const std::string zone =
            tenant == tenant_a ? "00000000-0000-4000-8000-0000000000b1" : "00000000-0000-4000-8000-0000000000bb";
        return "('" + std::string(id) + "', '" + std::string(tenant) + "', '" + provider + "', '" +
               zone + "', 'cert-" + std::string(id.substr(34)) + ".example', '" + std::string(status) +
               "', 1, 1, 1, " + std::string(expires) + ", " + std::string(error) + ", " +
               std::string(deleted) + ")";
    };
    co_await exec(
        "INSERT INTO sys_certificate (id, tenant_id, provider_id, dns_zone_id, domain, status, "
        " revision, issuance_revision, issued_revision, expires_at, "
        "last_error, deleted_at) VALUES " +
        certificate(cert_expiring, tenant_a, "valid", "NOW() + INTERVAL '10 days'", "NULL", "NULL") + ", " +
        certificate(cert_later, tenant_a, "valid", "NOW() + INTERVAL '90 days'", "NULL", "NULL") + ", " +
        certificate("00000000-0000-4000-8000-0000000000e3", tenant_a, "valid",
            "NOW() + INTERVAL '90 days'", "'renewal failed'", "NULL") +
        ", " +
        certificate("00000000-0000-4000-8000-0000000000e4", tenant_a, "failed", "NULL", "'acme'", "NULL") + ", " +
        certificate("00000000-0000-4000-8000-0000000000e5", tenant_a, "expired",
            "NOW() - INTERVAL '1 day'", "NULL", "NULL") +
        ", " +
        certificate("00000000-0000-4000-8000-0000000000e6", tenant_a, "pending", "NULL", "NULL", "NULL") + ", " +
        certificate("00000000-0000-4000-8000-0000000000e7", tenant_a, "valid",
            "NOW() + INTERVAL '5 days'", "NULL", "NOW()") +
        ", " +
        certificate("00000000-0000-4000-8000-0000000000eb", tenant_b, "valid",
            "NOW() + INTERVAL '5 days'", "NULL", "NULL"));

    // Websites 1..10 on cluster A; 11 is deleted. Website 10 has no name and
    // falls back to its first hostname; websites 2 and 5 do not log.
    std::string websites =
        "INSERT INTO sys_website (id, tenant_id, cluster_id, revision, config, deleted_at) VALUES ";
    for (int index = 1; index <= 11; ++index) {
        const auto id = "00000000-0000-4000-8000-000000000" + std::to_string(100 + index);
        const auto host = "site" + std::to_string(index) + ".example";
        const auto name = index == 1    ? std::optional<std::string>{"alpha"}
                          : index == 10 ? std::nullopt
                                        : std::optional<std::string>{"site-" + std::to_string(index)};
        if (index > 1) {
            websites += ", ";
        }
        websites += "('" + id + "', '" + std::string(tenant_a) + "', '" + std::string(cluster_a) +
                    "', 1, '" +
                    website_config(index, host,
                        name ? std::optional<std::string_view>{*name} : std::nullopt,
                        index != 2 && index != 5) +
                    "', " + (index == 11 ? "NOW()" : "NULL") + ")";
    }
    websites += ", ('00000000-0000-4000-8000-0000000001b1', '" + std::string(tenant_b) +
                "', '00000000-0000-4000-8000-0000000000cb', 1, '" +
                website_config(99, "b.example", "beta", true) + "', NULL)";
    co_await exec(websites);
    co_await exec(
        "INSERT INTO sys_website_certificate_binding (tenant_id, website_id, certificate_id, "
        "position) VALUES ('" +
        std::string(tenant_a) + "', '" + std::string(website1) + "', '" +
        std::string(cert_expiring) + "', 0), ('" + std::string(tenant_a) +
        "', '00000000-0000-4000-8000-000000000103', '" + std::string(cert_expiring) + "', 0), ('" +
        std::string(tenant_a) + "', '00000000-0000-4000-8000-000000000102', '" +
        std::string(cert_later) + "', 0), ('" + std::string(tenant_b) +
        "', '00000000-0000-4000-8000-0000000001b1', '00000000-0000-4000-8000-0000000000eb', 0)");

    // Access logs. Offsets are relative to the start of the current hour of
    // the transaction clock, so the buckets are deterministic.
    const auto log = [](std::string_view tenant, std::string_view node, std::string_view website,
                         std::string_view offset, int status, std::int64_t bytes,
                         std::string_view ip) {
        return "('" + std::string(tenant) + "', date_trunc('hour', NOW()) + INTERVAL '" +
               std::string(offset) + "', '" + std::string(node) + "', '" + std::string(website) +
               "', " + (ip.empty() ? std::string("NULL") : "'" + std::string(ip) + "'") +
               ", 'http/1.1', 'GET', 'site.example', '/', " + std::to_string(status) + ", " +
               std::to_string(bytes) + ")";
    };
    std::string logs =
        "INSERT INTO sys_website_access_log (tenant_id, occurred_at, node_id, "
        "website_id, client_ip, protocol, method, host, target, status_code, "
        "response_bytes) VALUES ";
    logs += log(tenant_a, node_online, website1, "1 minute", 200, 1000, "10.0.0.1") + ", ";
    logs += log(tenant_a, node_online, website1, "1 minute 30 seconds", 200, 2000, "10.0.0.2") + ", ";
    logs += log(tenant_a, node_online, "00000000-0000-4000-8000-000000000102", "-23 hours 2 minutes",
                404, 300, "10.0.0.1") +
            ", ";
    // Before the window: excluded everywhere.
    logs += log(tenant_a, node_online, "00000000-0000-4000-8000-000000000102", "-23 hours -1 minute",
                500, 100000, "10.9.9.9") +
            ", ";
    for (int index = 3; index <= 10; ++index) {
        const auto status = index == 3 ? 301 : index == 4 ? 101
                                           : index == 5   ? 502
                                                          : 200;
        logs += log(tenant_a, node_online,
                    "00000000-0000-4000-8000-000000000" + std::to_string(100 + index),
                    "-5 hours 10 minutes", status, 100 * index,
                    index == 4 ? "" : "10.0.1." + std::to_string(index)) +
                ", ";
    }
    logs += log(tenant_a, node_online, website_deleted, "-1 hour", 503, 5000, "10.0.0.9") + ", ";
    logs += log(tenant_b, "00000000-0000-4000-8000-0000000000b9", "00000000-0000-4000-8000-0000000001b1",
        "5 minutes", 500, 99999, "10.0.0.1");
    co_await exec(logs);
}

std::int64_t integer(const auto& value) {
    return value.value_;
}

template <typename page_type>
ruvia::task<std::int64_t> total_of(ruvia::task<page_type> page) {
    const auto value = co_await std::move(page);
    co_return value.template get<"total">().value_;
}

void require_not_found(const ruvia::http_error& error, std::string_view code) {
    REQUIRE(error.info().status().value() == 404);
    REQUIRE(error.info().code() == code);
}

ruvia::task<void> verify_traffic(read_context& c, ruvia::db_transaction& tx) {
    const auto hour_rows = co_await tx.query(
        "SELECT to_char(date_trunc('hour', NOW()), 'YYYY-MM-DD\"T\"HH24:MI:SSOF'), "
        "to_char(date_trunc('hour', NOW()) - INTERVAL '23 hours', 'YYYY-MM-DD\"T\"HH24:MI:SSOF')");
    const auto current_hour = std::string(hour_rows.front()[0].value().value_or(""));
    const auto first_hour = std::string(hour_rows.front()[1].value().value_or(""));

    const auto traffic = co_await service::overview::get_overview_traffic_service().get(c, std::string(tenant_a));
    REQUIRE(integer(traffic.get<"window_hours">()) == 24);
    const auto& hourly = traffic.get<"hourly">();
    REQUIRE(hourly.size() == 24);
    REQUIRE(hourly.front().get<"timestamp">().view() == first_hour);
    REQUIRE(hourly.back().get<"timestamp">().view() == current_hour);
    for (std::size_t index = 1; index < hourly.size(); ++index) {
        REQUIRE(hourly[index - 1].get<"timestamp">().view() < hourly[index].get<"timestamp">().view());
    }
    // Buckets and zero fill.
    REQUIRE(integer(hourly[23].get<"request_count">()) == 2);
    REQUIRE(integer(hourly[23].get<"response_bytes">()) == 3000);
    REQUIRE(integer(hourly[23].get<"bandwidth_bps">()) == 7);
    REQUIRE(integer(hourly[0].get<"request_count">()) == 1);
    REQUIRE(integer(hourly[0].get<"response_bytes">()) == 300);
    REQUIRE(integer(hourly[18].get<"request_count">()) == 8);
    REQUIRE(integer(hourly[18].get<"response_bytes">()) == 5200);
    REQUIRE(integer(hourly[22].get<"response_bytes">()) == 5000);
    for (const auto index : {1, 5, 17, 19, 21}) {
        REQUIRE(integer(hourly[static_cast<std::size_t>(index)].get<"request_count">()) == 0 &&
                integer(hourly[static_cast<std::size_t>(index)].get<"response_bytes">()) == 0);
    }
    // Summary covers every logged request in the window, deleted websites included.
    const auto& summary = traffic.get<"summary">();
    REQUIRE(integer(summary.get<"request_count">()) == 12);
    REQUIRE(integer(summary.get<"response_bytes">()) == 13500);
    REQUIRE(integer(summary.get<"status5xx_count">()) == 2);
    REQUIRE(integer(summary.get<"unique_client_ips">()) == 10);
    // Peak of per-minute bandwidth: 5200 bytes in one minute = 693 bps.
    REQUIRE(integer(summary.get<"peak_bandwidth_bps">()) == 693);
    // Status classes: fixed 1xx..5xx order.
    const auto& classes = traffic.get<"status_classes">();
    REQUIRE(classes.size() == 5);
    const std::int64_t expected_classes[]{1, 7, 1, 1, 2};
    const std::string_view labels[]{"1xx", "2xx", "3xx", "4xx", "5xx"};
    for (std::size_t index = 0; index < 5; ++index) {
        REQUIRE(classes[index].get<"label">().view() == labels[index]);
        REQUIRE(integer(classes[index].get<"request_count">()) == expected_classes[index]);
    }
    // Top 8 by bytes; deleted website 11 (5000 bytes) is excluded.
    const auto& top = traffic.get<"top_websites">();
    REQUIRE(top.size() == 8);
    REQUIRE(top[0].get<"website_id">().view() == website1);
    REQUIRE(top[0].get<"name">().view() == "alpha");
    REQUIRE(integer(top[0].get<"request_count">()) == 2);
    REQUIRE(integer(top[0].get<"response_bytes">()) == 3000);
    REQUIRE(top[1].get<"website_id">().view() == website10);
    REQUIRE(top[1].get<"name">().view() == "site10.example");
    for (std::size_t index = 1; index < top.size(); ++index) {
        REQUIRE(integer(top[index - 1].get<"response_bytes">()) >=
                integer(top[index].get<"response_bytes">()));
    }
    REQUIRE(integer(top.back().get<"response_bytes">()) == 400);
    for (const auto& item : top) {
        REQUIRE(item.get<"website_id">().view() != website_deleted);
    }
    // Coverage: undeleted websites, and those with access logs enabled.
    const auto& coverage = traffic.get<"coverage">();
    REQUIRE(integer(coverage.get<"website_count">()) == 10);
    REQUIRE(integer(coverage.get<"logging_website_count">()) == 8);

    // Tenant isolation.
    const auto other = co_await service::overview::get_overview_traffic_service().get(c, std::string(tenant_b));
    REQUIRE(integer(other.get<"summary">().get<"request_count">()) == 1);
    REQUIRE(integer(other.get<"summary">().get<"response_bytes">()) == 99999);
    REQUIRE(other.get<"top_websites">().size() == 1);
    REQUIRE(integer(other.get<"coverage">().get<"website_count">()) == 1);
    const auto empty = co_await service::overview::get_overview_traffic_service().get(c, std::string(tenant_c));
    REQUIRE(empty.get<"hourly">().size() == 24);
    for (const auto& point : empty.get<"hourly">()) {
        REQUIRE(integer(point.get<"request_count">()) == 0 && integer(point.get<"bandwidth_bps">()) == 0);
    }
    REQUIRE(empty.get<"status_classes">().size() == 5);
    REQUIRE(empty.get<"top_websites">().empty());
    REQUIRE(integer(empty.get<"summary">().get<"peak_bandwidth_bps">()) == 0);
    REQUIRE(integer(empty.get<"coverage">().get<"website_count">()) == 0);
}

ruvia::task<void> verify_overview_and_filters(read_context& c) {
    const std::string tenant(tenant_a);
    auto overview = co_await service::overview::get_overview_service().get(c, tenant);
    const auto& resources = overview.get<"resources">();
    const auto& issues = overview.get<"issues">();
    // node-5 is deleted; node-1 is the only registered, enabled, recent node.
    REQUIRE(integer(resources.get<"node_count">()) == 4);
    REQUIRE(integer(resources.get<"online_node_count">()) == 1);
    REQUIRE(integer(issues.get<"dns_zone_issue_count">()) == 2);
    REQUIRE(integer(issues.get<"certificate_expiring_count">()) == 1);
    REQUIRE(integer(issues.get<"certificate_failed_count">()) == 3);

    // DNS zones: `issue` equals the overview count; single statuses filter exactly.
    // Coroutine lambdas keep their arguments alive across the lazy read.
    const auto zones = [&](std::optional<std::string> status)
        -> ruvia::task<service::dns_zone::dns_zone_page_data_dto> {
        co_return co_await service::dns_zone::get_dns_zone_read_service().list(
            c, tenant, 1, 100, 0, std::nullopt, std::nullopt, status);
    };
    const auto issue = co_await zones(std::string("issue"));
    REQUIRE(integer(issue.get<"total">()) == integer(issues.get<"dns_zone_issue_count">()));
    for (const auto& zone : issue.get<"list">()) {
        REQUIRE(zone.get<"sync_status">().view() == "failed" || zone.get<"sync_status">().view() == "conflict");
    }
    const auto synced = co_await zones(std::string("synced"));
    REQUIRE(integer(synced.get<"total">()) == 1 && synced.get<"list">()[0].get<"domain">().view() == "a.example");
    REQUIRE((co_await total_of(zones(std::string("pending")))) == 1);
    REQUIRE((co_await total_of(zones(std::nullopt))) == 4);
    // Tenant B has one failed zone and none of tenant A's.
    REQUIRE((co_await total_of(service::dns_zone::get_dns_zone_read_service().list(
                c, std::string(tenant_b), 1, 100, 0, std::nullopt, std::nullopt,
                std::string("issue")))) == 1);

    // Certificates: the filters equal the overview counts.
    const auto certificates = [&](bool expiring, bool abnormal)
        -> ruvia::task<service::certificate::certificate_page_data_dto> {
        co_return co_await service::certificate::get_certificate_read_service().list(
            c, tenant, 1, 100, 0, std::nullopt, std::nullopt, std::nullopt, expiring, abnormal);
    };
    const auto expiring = co_await certificates(true, false);
    REQUIRE(integer(expiring.get<"total">()) == integer(issues.get<"certificate_expiring_count">()));
    REQUIRE(expiring.get<"list">()[0].get<"id">().view() == cert_expiring);
    const auto abnormal = co_await certificates(false, true);
    REQUIRE(integer(abnormal.get<"total">()) == integer(issues.get<"certificate_failed_count">()));
    for (const auto& item : abnormal.get<"list">()) {
        REQUIRE(item.get<"id">().view() != cert_expiring && item.get<"id">().view() != cert_later);
    }
    REQUIRE((co_await total_of(certificates(true, true))) == 0);
    REQUIRE((co_await total_of(certificates(false, false))) == 6);

    // Websites bound to a certificate.
    const auto websites = [&](std::string_view tenant_id, std::string_view certificate_id)
        -> ruvia::task<service::website::website_page_data_dto> {
        co_return co_await service::website::get_website_read_service().list(
            c, std::string(tenant_id), 1, 100, 0, std::nullopt, std::nullopt, std::nullopt,
            std::string(certificate_id));
    };
    const auto bound = co_await websites(tenant_a, cert_expiring);
    REQUIRE(integer(bound.get<"total">()) == 2);
    for (const auto& item : bound.get<"list">()) {
        REQUIRE(item.get<"id">().view() == website1 ||
                item.get<"id">().view() == "00000000-0000-4000-8000-000000000103");
    }
    REQUIRE((co_await total_of(websites(tenant_a, cert_later))) == 1);
    REQUIRE((co_await total_of(websites(tenant_a, unknown_id))) == 0);
    REQUIRE((co_await total_of(websites(tenant_b, cert_expiring))) == 0);
}

ruvia::task<void> verify_details(read_context& c) {
    const std::string tenant(tenant_a);
    // Node detail equals the node's list item byte for byte.
    auto page = co_await service::node::get_node_read_service().list(c, tenant, 1, 100, 0, std::nullopt,
        std::nullopt, std::nullopt,
        std::nullopt, std::nullopt);
    std::string listed;
    for (const auto& item : page.get<"list">()) {
        if (item.get<"id">().view() == node_online) {
            listed = std::string(ruvia::to_json(item));
        }
    }
    const auto node = co_await service::node::get_node_read_service().get(c, tenant, std::string(node_online));
    REQUIRE(!listed.empty() && std::string(ruvia::to_json(node)) == listed);
    REQUIRE(node.get<"runtime">().get<"connection_status">().view() == "online");
    try {
        (void)co_await service::node::get_node_read_service().get(c, std::string(tenant_b),
            std::string(node_online));
        REQUIRE(false);
    } catch (const ruvia::http_error& error) {
        require_not_found(error, "16501");
    }
    try {
        (void)co_await service::node::get_node_read_service().get(
            c, tenant, "00000000-0000-4000-8000-0000000000a5");
        REQUIRE(false);
    } catch (const ruvia::http_error& error) {
        require_not_found(error, "16501");
    }

    auto clusters = co_await service::cluster::get_cluster_read_service().list(c, tenant, 1, 100, 0,
        std::nullopt, std::nullopt,
        std::nullopt);
    REQUIRE(clusters.get<"list">().size() == 1);
    const auto cluster = co_await service::cluster::get_cluster_read_service().get(c, tenant, std::string(cluster_a));
    REQUIRE(std::string(ruvia::to_json(cluster)) == std::string(ruvia::to_json(clusters.get<"list">()[0])));
    REQUIRE(integer(cluster.get<"node_count">()) == 4 && integer(cluster.get<"online_node_count">()) == 1);
    try {
        (void)co_await service::cluster::get_cluster_read_service().get(c, std::string(tenant_b),
            std::string(cluster_a));
        REQUIRE(false);
    } catch (const ruvia::http_error& error) {
        require_not_found(error, "16401");
    }
}

template <typename parse_type>
void require_bad_request(parse_type parse) {
    try {
        parse();
    } catch (const ruvia::http_error& error) {
        REQUIRE(error.info().status().value() == 400);
        REQUIRE(error.info().code() == "10001");
        return;
    }
    throw std::runtime_error("invalid filter value was accepted");
}

void verify_filter_parsing() {
    using service::dns_zone::parse_sync_status_filter;
    REQUIRE(!parse_sync_status_filter(std::nullopt));
    REQUIRE(parse_sync_status_filter("issue") == "issue");
    REQUIRE(parse_sync_status_filter("conflict") == "conflict");
    require_bad_request([] { (void)parse_sync_status_filter("syncing"); });
    require_bad_request([] { (void)parse_sync_status_filter(""); });

    using service::certificate::parse_attention_filter;
    REQUIRE(!parse_attention_filter(std::nullopt, "expiring"));
    REQUIRE(parse_attention_filter("1", "expiring"));
    require_bad_request([] { (void)parse_attention_filter("true", "expiring"); });
    require_bad_request([] { (void)parse_attention_filter("0", "abnormal"); });

    using service::website::parse_certificate_id_filter;
    REQUIRE(!parse_certificate_id_filter(std::nullopt));
    REQUIRE(parse_certificate_id_filter(cert_expiring) == std::string(cert_expiring));
    require_bad_request([] { (void)parse_certificate_id_filter("not-a-uuid"); });
}

// Without an installed GeoIP database the country ranking is still part of the
// dashboard contract: an empty array, not an omitted required field.
ruvia::task<void> verify_dashboard_without_geoip(read_context& c) {
    REQUIRE(!service::geoip::get_xdb_database().available());
    const auto dashboard = co_await service::website::get_website_dashboard_service().dashboard(
        c, std::string(tenant_a), std::string(website1));
    const std::string json(ruvia::to_json(dashboard));
    REQUIRE(json.find("\"countries\":[]") != std::string::npos);
}

ruvia::task<void> verify(ruvia::db_client& client) {
    verify_filter_parsing();
    co_await client.connect();
    auto db = client.handle();
    auto tx = co_await db.begin_transaction();
    std::pmr::unsynchronized_pool_resource resource;
    read_context c{&tx, &resource};
    co_await apply_schema(tx);
    co_await insert_fixture(tx);
    co_await verify_traffic(c, tx);
    co_await verify_overview_and_filters(c);
    co_await verify_details(c);
    co_await verify_dashboard_without_geoip(c);
    co_await tx.rollback();
}

}  // namespace

int main() {
    // The dashboard check needs the "no GeoIP database installed" configuration.
#ifdef _WIN32
    _putenv_s("FLEXEDGE_XDB_V4_PATH", "");
    _putenv_s("FLEXEDGE_XDB_V6_PATH", "");
#else
    ::unsetenv("FLEXEDGE_XDB_V4_PATH");
    ::unsetenv("FLEXEDGE_XDB_V6_PATH");
#endif
    return test_support::run_postgres_test(verify);
}
