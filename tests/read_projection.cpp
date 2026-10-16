// Read projections over the real schema: the tenant traffic aggregate, the
// overview node counts, node/cluster detail reads and the list filters that
// share their definitions with the overview counts. The schema is applied and
// populated inside one transaction that is rolled back.
#include <ruvia/web/db/db_transaction.h>
#include <ruvia/web/error.h>
#include <ruvia/web/model_json.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "service/config/geoip.h"
#include "service/config/schema.h"
#include "service/domains/certificate/certificate_filter.h"
#include "service/domains/certificate/certificate_read_service.h"
#include "service/domains/cluster/cluster_read_service.h"
#include "service/domains/dns_zone/dns_zone_filter.h"
#include "service/domains/dns_zone/dns_zone_read_service.h"
#include "service/domains/node/node_read_service.h"
#include "service/domains/overview/overview_service.h"
#include "service/domains/overview/overview_traffic_service.h"
#include "service/domains/provider/certificate_provider_read_service.h"
#include "service/domains/provider/dns_provider_read_service.h"
#include "service/domains/website/website_dashboard_service.h"
#include "service/domains/website/website_filter.h"
#include "service/domains/website/website_read_service.h"
#include "service/features/geoip/runtime_database.h"
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
    std::size_t* query_count_;
    template <typename... args_type>
    auto query(args_type&&... args) const {
        ++*query_count_;
        return transaction_->query(std::forward<args_type>(args)...);
    }
};

struct read_context final {
    ruvia::db_transaction* transaction_;
    std::pmr::memory_resource* resource_;
    std::size_t query_count_{};
    transaction_db db() {
        return {transaction_, &query_count_};
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
    co_await exec(R"sql(UPDATE sys_provider SET config =
        '{"credential":{"envelope":"qa-opaque","hint":"****qa"}}' WHERE kind = 'dns')sql");
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

ruvia::task<void> verify_provider_pages(read_context& c, ruvia::db_transaction& tx) {
    // Respect the production uniqueness constraint: two supported CAs, one each.
    (void)co_await tx.execute(
        R"(UPDATE sys_provider SET config = '{"credential_mode":"email","account_email":"ops+100%_@example.com"}'
           WHERE kind = 'certificate')");
    (void)co_await tx.execute(
        R"(INSERT INTO sys_provider (id, tenant_id, kind, provider, status, config, deleted_at) VALUES
           ('00000000-0000-4000-8000-0000000000f2', '00000000-0000-4000-8000-00000000000a',
            'certificate', 'zerossl', 'unverified',
            '{"credential_mode":"access_key","access_key":{"envelope":"opaque-secret","hint":"****ZX_9"}}', NULL),
           ('00000000-0000-4000-8000-0000000000f3', '00000000-0000-4000-8000-00000000000a',
            'certificate', 'letsencrypt', 'invalid',
            '{"credential_mode":"email","account_email":"deleted@example.com"}', NOW()))");
    const std::string tenant(tenant_a);
    const auto& providers = service::provider::get_certificate_provider_read_service();
    const auto first = co_await providers.list(c, tenant, 1, 1, 0, std::nullopt);
    const auto second = co_await providers.list(c, tenant, 2, 1, 1, std::nullopt);
    REQUIRE(integer(first.get<"total">()) == 2);
    REQUIRE(integer(first.get<"page">()) == 1);
    REQUIRE(integer(first.get<"page_size">()) == 1);
    REQUIRE(integer(first.get<"total_pages">()) == 2);
    REQUIRE(first.get<"list">().size() == 1 && second.get<"list">().size() == 1);
    REQUIRE(first.get<"list">()[0].get<"provider">().view() == "zerossl");
    REQUIRE(second.get<"list">()[0].get<"provider">().view() == "letsencrypt");
    REQUIRE(integer(second.get<"page">()) == 2);
    REQUIRE(integer(second.get<"total">()) == 2);
    const auto past_end = co_await providers.list(c, tenant, 3, 1, 2, std::nullopt);
    REQUIRE(past_end.get<"list">().empty());
    REQUIRE(integer(past_end.get<"total">()) == 2);
    REQUIRE(integer(past_end.get<"total_pages">()) == 2);
    const auto search = [&](std::string keyword)
        -> ruvia::task<service::provider::certificate_provider_page_data_dto> {
        co_return co_await providers.list(c, tenant, 1, 1, 0, keyword);
    };
    for (const auto keyword : {"letsencrypt", "LET'S ENCRYPT", "ops+100", "%", "ZeroSSL", "ZX_9"}) {
        const auto found = co_await search(keyword);
        REQUIRE(integer(found.get<"total">()) == 1);
        REQUIRE(integer(found.get<"total_pages">()) == 1);
        REQUIRE(found.get<"list">().size() == 1);
    }
    for (const auto keyword : {"ZX%9", "deleted@example.com", "opaque-secret", "missing"}) {
        const auto empty = co_await search(keyword);
        REQUIRE(empty.get<"list">().empty());
        REQUIRE(integer(empty.get<"total">()) == 0);
        REQUIRE(integer(empty.get<"total_pages">()) == 0);
    }
    const auto other = co_await providers.list(c, std::string(tenant_b), 1, 1, 0, std::nullopt);
    REQUIRE(integer(other.get<"total">()) == 1);
    REQUIRE(other.get<"list">()[0].get<"id">().view() == "00000000-0000-4000-8000-0000000000fb");
    const auto empty = co_await providers.list(c, std::string(tenant_c), 1, 1, 0, std::nullopt);
    REQUIRE(empty.get<"list">().empty());
    REQUIRE(integer(empty.get<"total">()) == 0);
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

// Every projection uses the 15 second window of node_connection.h. The fixture runs in one
// transaction, so NOW() is the same for the update and the reads.
ruvia::task<void> verify_online_window(read_context& c, ruvia::db_transaction& tx) {
    const std::string tenant(tenant_a);
    const auto online_total = [&](std::string_view status) -> ruvia::task<std::int64_t> {
        co_return co_await total_of(service::node::get_node_read_service().list(c, tenant, 1, 100, 0,
            std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::string(status)));
    };
    for (const auto& [age, online] : {std::pair{"14 seconds", true}, std::pair{"16 seconds", false}}) {
        (void)co_await tx.execute("UPDATE sys_node SET last_heartbeat_at = NOW() - INTERVAL '" +
                                  std::string(age) + "' WHERE id = '" + std::string(node_online) + "'");
        const auto node = co_await service::node::get_node_read_service().get(c, tenant, std::string(node_online));
        REQUIRE(node.get<"runtime">().get<"connection_status">().view() == (online ? "online" : "offline"));
        REQUIRE((co_await online_total("online")) == (online ? 1 : 0));
        // node-2 (disabled) and node-4 (ten minutes old) are offline throughout.
        REQUIRE((co_await online_total("offline")) == (online ? 2 : 3));
        const auto cluster = co_await service::cluster::get_cluster_read_service().get(c, tenant, std::string(cluster_a));
        REQUIRE(integer(cluster.get<"online_node_count">()) == (online ? 1 : 0));
        auto overview = co_await service::overview::get_overview_service().get(c, tenant);
        REQUIRE(integer(overview.get<"resources">().get<"online_node_count">()) == (online ? 1 : 0));
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

ruvia::task<void> verify_dashboard_asns(read_context& c, ruvia::db_transaction& tx) {
    constexpr std::string_view website{"00000000-0000-4000-8000-000000000113"};
    constexpr std::string_view empty_website{"00000000-0000-4000-8000-000000000114"};
    constexpr std::string_view other_website{"00000000-0000-4000-8000-000000000115"};
    constexpr std::string_view as_of{"2026-10-03T12:00:00Z"};
    for (const auto& [id, number] : {
             std::pair{website, 13}, std::pair{empty_website, 14}, std::pair{other_website, 15}}) {
        (void)co_await tx.execute(
            "INSERT INTO sys_website (id, tenant_id, cluster_id, revision, config) VALUES ('" +
            std::string(id) + "', '" + std::string(tenant_a) + "', '" + std::string(cluster_a) +
            "', 1, '" + website_config(number, "asn.example", "ASN fixture", true) + "')");
    }

    std::string logs;
    const auto add_log = [&](std::string_view ip, std::int64_t bytes,
                             std::string_view log_website = {},
                             std::string_view tenant = tenant_a,
                             std::string_view node = node_online,
                             std::string_view occurred_at = "2026-10-03T11:00:00Z") {
        if (!logs.empty()) {
            logs += ", ";
        }
        logs += "('" + std::string(tenant) + "', '" + std::string(occurred_at) + "', '" +
                std::string(node) + "', '" + std::string(log_website.empty() ? website : log_website) + "', " +
                (ip.empty() ? std::string{"NULL"} : "'" + std::string(ip) + "'") +
                ", 'http/1.1', 'GET', 'asn.example', '/', 200, " + std::to_string(bytes) + ")";
    };
    // Twelve IPs each have two requests. They exclude every AS4134 IP from both
    // existing top-10 client-IP rankings, but the four combined requests win by ASN.
    for (int index = 10; index <= 21; ++index) {
        const auto ip = "1.2." + std::to_string(index) + ".1";
        const auto bytes = index == 21 ? 500 : 300;
        add_log(ip, bytes);
        add_log(ip, bytes);
    }
    add_log("1.2.3.1", 100);
    add_log("1.2.7.1", 200);
    add_log("1.2.8.1", 300);
    add_log("2001:db8::1", 400);
    // A missing address, missing ASN metadata and an unmapped address share one bucket.
    add_log("", 11);
    add_log("1.2.4.4", 13);
    add_log("1.2.250.1", 17);
    // These rows must never contribute: old window, another website, another tenant.
    add_log("1.2.3.1", 100000, website, tenant_a, node_online, "2026-10-02T11:59:59Z");
    add_log("1.2.3.1", 200000, other_website);
    add_log("1.2.3.1", 300000, "00000000-0000-4000-8000-0000000001b1", tenant_b,
        "00000000-0000-4000-8000-0000000000b9");
    (void)co_await tx.execute(
        "INSERT INTO sys_website_access_log (tenant_id, occurred_at, node_id, website_id, "
        "client_ip, protocol, method, host, target, status_code, response_bytes) VALUES " +
        logs);

    const auto dashboard = co_await service::website::get_website_dashboard_service().dashboard(
        c, std::string(tenant_a), std::string(website), as_of);
    const auto& asns = dashboard.get<"asns">();
    if (service::geoip::get_xdb_database().available()) {
        REQUIRE(asns.size() == 10);
        REQUIRE(asns[0].get<"label">().view() == "AS4134 Chinanet");
        REQUIRE(integer(asns[0].get<"request_count">()) == 4);
        REQUIRE(integer(asns[0].get<"response_bytes">()) == 1000);
        REQUIRE(asns[1].get<"label">().view() == "未知 ASN");
        REQUIRE(integer(asns[1].get<"request_count">()) == 3);
        REQUIRE(integer(asns[1].get<"response_bytes">()) == 41);
        // Response bytes break request-count ties; equal totals use the full label.
        REQUIRE(asns[2].get<"label">().view() ==
                "AS65021 A Complete Autonomous System Organization Name");
        REQUIRE(integer(asns[2].get<"response_bytes">()) == 1000);
        for (std::size_t index = 3; index < asns.size(); ++index) {
            REQUIRE(asns[index].get<"label">().view() ==
                    "AS650" + std::to_string(index + 7) + " Network " + std::to_string(index + 7));
            REQUIRE(integer(asns[index].get<"request_count">()) == 2);
            REQUIRE(integer(asns[index].get<"response_bytes">()) == 600);
        }
        REQUIRE(dashboard.get<"countries">().size() == 2);
        REQUIRE(dashboard.get<"countries">()[0].get<"label">().view() == "中国");
        REQUIRE(integer(dashboard.get<"countries">()[0].get<"request_count">()) == 28);
    } else {
        REQUIRE(asns.size() == 1);
        REQUIRE(asns[0].get<"label">().view() == "未知 ASN");
        REQUIRE(integer(asns[0].get<"request_count">()) == 31);
        REQUIRE(integer(asns[0].get<"response_bytes">()) == 8641);
        REQUIRE(dashboard.get<"countries">().empty());
    }
    for (const auto& ranking : {std::cref(dashboard.get<"client_ips_by_bytes">()),
             std::cref(dashboard.get<"client_ips_by_requests">())}) {
        REQUIRE(ranking.get().size() == 10);
        for (const auto& ip : ranking.get()) {
            REQUIRE(ip.get<"asn">().view() != "AS4134");
        }
    }
    const auto empty = co_await service::website::get_website_dashboard_service().dashboard(
        c, std::string(tenant_a), std::string(empty_website), as_of);
    REQUIRE(empty.get<"asns">().empty());
    REQUIRE(std::string(ruvia::to_json(empty)).find("\"asns\":[]") != std::string::npos);
    try {
        (void)co_await service::website::get_website_dashboard_service().dashboard(
            c, std::string(tenant_b), std::string(website), as_of);
        REQUIRE(false);
    } catch (const ruvia::http_error& error) {
        require_not_found(error, "16601");
    }
}

// Daily buckets are console days (Asia/Shanghai), like 今日: as of 2026-10-02
// 02:00 +08, a request at 10-01 23:30 +08 (15:30 UTC) and one at 10-02 00:30 +08
// (16:30 UTC) fall into different buckets, and today's bucket equals 今日.
ruvia::task<void> verify_dashboard_console_days(read_context& c, ruvia::db_transaction& tx) {
    constexpr std::string_view website{"00000000-0000-4000-8000-000000000112"};
    (void)co_await tx.execute(
        "INSERT INTO sys_website (id, tenant_id, cluster_id, revision, config, deleted_at) "
        "VALUES ('" +
        std::string(website) + "', '" + std::string(tenant_a) + "', '" + std::string(cluster_a) +
        "', 1, '" + website_config(12, "site12.example", "console-day", true) + "', NULL)");
    const auto log = [&](std::string_view occurred_at, std::int64_t bytes) {
        return "('" + std::string(tenant_a) + "', '" + std::string(occurred_at) + "', '" +
               std::string(node_online) + "', '" + std::string(website) +
               "', '10.0.2.1', 'http/1.1', 'GET', 'site12.example', '/', 200, " +
               std::to_string(bytes) + ")";
    };
    (void)co_await tx.execute(
        "INSERT INTO sys_website_access_log (tenant_id, occurred_at, node_id, website_id, "
        "client_ip, protocol, method, host, target, status_code, response_bytes) VALUES " +
        log("2026-10-01T15:30:00Z", 700) + ", " + log("2026-10-01T16:30:00Z", 900));

    const auto dashboard = co_await service::website::get_website_dashboard_service().dashboard(
        c, std::string(tenant_a), std::string(website), "2026-10-01T18:00:00Z");
    const auto& daily = dashboard.get<"daily">();
    REQUIRE(daily.size() == 15);
    // Bucket starts are console midnights, rendered like every other timestamp.
    const auto midnights = co_await tx.query(
        "SELECT TO_CHAR('2026-10-01T00:00:00+08:00'::timestamptz, 'YYYY-MM-DD\"T\"HH24:MI:SSOF'), "
        "TO_CHAR('2026-10-02T00:00:00+08:00'::timestamptz, 'YYYY-MM-DD\"T\"HH24:MI:SSOF'), "
        "TO_CHAR('2026-09-18T00:00:00+08:00'::timestamptz, 'YYYY-MM-DD\"T\"HH24:MI:SSOF')");
    REQUIRE(daily[13].get<"timestamp">().view() == midnights.front()[0].value().value_or(""));
    REQUIRE(daily[14].get<"timestamp">().view() == midnights.front()[1].value().value_or(""));
    REQUIRE(daily[0].get<"timestamp">().view() == midnights.front()[2].value().value_or(""));
    REQUIRE(integer(daily[13].get<"request_count">()) == 1);
    REQUIRE(integer(daily[13].get<"response_bytes">()) == 700);
    REQUIRE(integer(daily[14].get<"request_count">()) == 1);
    REQUIRE(integer(daily[14].get<"response_bytes">()) == 900);
    for (std::size_t index = 0; index < 13; ++index) {
        REQUIRE(integer(daily[index].get<"request_count">()) == 0);
    }
    const auto& summary = dashboard.get<"summary">();
    REQUIRE(integer(summary.get<"today_response_bytes">()) ==
            integer(daily[14].get<"response_bytes">()));
    REQUIRE(integer(summary.get<"today_unique_ips">()) == 1);
}

// Options use exactly the list projection, with no count or page boundary.
ruvia::task<void> verify_options(read_context& c, ruvia::db_transaction& tx) {
    const auto& clusters = service::cluster::get_cluster_read_service();
    const auto& certificates = service::certificate::get_certificate_read_service();
    const auto& dns = service::provider::get_dns_provider_read_service();
    const auto& issuers = service::provider::get_certificate_provider_read_service();
    for (const auto identity : {tenant_a, tenant_b, tenant_c}) {
        const std::string tenant(identity);
        auto before = c.query_count_;
        const auto cluster_options = co_await clusters.options(c, tenant, std::nullopt);
        REQUIRE(c.query_count_ == before + 1);
        const auto cluster_page = co_await clusters.list(c, tenant, 1, 10000, 0, std::nullopt, std::nullopt, std::nullopt);
        REQUIRE(std::string(ruvia::to_json(cluster_options)) == std::string(ruvia::to_json(cluster_page.get<"list">())));
        before = c.query_count_;
        const auto certificate_options = co_await certificates.options(c, tenant, std::nullopt, std::nullopt, std::nullopt);
        REQUIRE(c.query_count_ == before + 1);
        const auto certificate_page = co_await certificates.list(c, tenant, 1, 10000, 0, std::nullopt, std::nullopt, std::nullopt, false, false);
        REQUIRE(std::string(ruvia::to_json(certificate_options)) == std::string(ruvia::to_json(certificate_page.get<"list">())));
        before = c.query_count_;
        const auto dns_options = co_await dns.options(c, tenant, std::nullopt, std::nullopt);
        REQUIRE(c.query_count_ == before + 1);
        const auto dns_page = co_await dns.list(c, tenant, 1, 10000, 0, std::nullopt, std::nullopt);
        REQUIRE(std::string(ruvia::to_json(dns_options)) == std::string(ruvia::to_json(dns_page.get<"list">())));
        before = c.query_count_;
        const auto issuer_options = co_await issuers.options(c, tenant, std::nullopt);
        REQUIRE(c.query_count_ == before + 1);
        const auto issuer_page = co_await issuers.list(c, tenant, 1, 10000, 0, std::nullopt);
        REQUIRE(std::string(ruvia::to_json(issuer_options)) == std::string(ruvia::to_json(issuer_page.get<"list">())));
    }
    for (const auto& keyword : {std::string("%"), std::string("missing")}) {
        const auto options = co_await issuers.options(c, std::string(tenant_a), keyword);
        const auto page = co_await issuers.list(c, std::string(tenant_a), 1, 100, 0, keyword);
        REQUIRE(std::string(ruvia::to_json(options)) == std::string(ruvia::to_json(page.get<"list">())));
    }
    (void)co_await tx.execute("SAVEPOINT option_boundaries");
    (void)co_await tx.execute(
        "INSERT INTO sys_sync_task (tenant_id, resource_type, certificate_id, operation, version, processed_version, is_done, is_ok, count_fails, error) "
        "SELECT tenant_id, 'certificate', id, 'issue', issuance_revision, issuance_revision, true, false, 1, 'permanent failure' "
        "FROM sys_certificate WHERE id = $1",
        cert_expiring);
    const auto terminal = co_await certificates.get(c, std::string(tenant_a), std::string(cert_expiring));
    REQUIRE(terminal.get<"sync_status">()->view() == "failed");
    const std::string tenant(tenant_a);
    for (const auto end : {1000, 1001}) {
        const int begin = end == 1000 ? 1 : 1001;
        (void)co_await tx.execute(
            "INSERT INTO sys_cluster (tenant_id, dns_zone_id, name, hostname_prefix) "
            "SELECT $1, '00000000-0000-4000-8000-0000000000b1', 'option-boundary-' || n, 'option-' || n "
            "FROM generate_series($2::int, $3::int) n",
            tenant, begin, end);
        (void)co_await tx.execute(
            "INSERT INTO sys_provider (tenant_id, kind, provider, name, account_id, status, config) "
            "SELECT $1, 'dns', 'cloudflare', 'option-boundary-' || n, 'option-' || n, 'verified', "
            "'{\"credential\":{\"envelope\":\"qa-opaque\",\"hint\":\"****qa\"}}'::jsonb "
            "FROM generate_series($2::int, $3::int) n",
            tenant, begin, end);
        (void)co_await tx.execute(
            "INSERT INTO sys_certificate (tenant_id, dns_zone_id, provider_id, domain) "
            "SELECT $1, '00000000-0000-4000-8000-0000000000b1', '00000000-0000-4000-8000-0000000000fa', "
            "'option-boundary-' || n || '.example' FROM generate_series($2::int, $3::int) n",
            tenant, begin, end);
        auto before = c.query_count_;
        const auto cluster_options = co_await clusters.options(c, tenant, std::string("option-boundary-"));
        REQUIRE(c.query_count_ == before + 1 && cluster_options.size() == static_cast<std::size_t>(end));
        const auto cluster_page = co_await clusters.list(c, tenant, 1, 10000, 0, std::string("option-boundary-"), std::nullopt, std::nullopt);
        REQUIRE(std::string(ruvia::to_json(cluster_options)) == std::string(ruvia::to_json(cluster_page.get<"list">())));
        before = c.query_count_;
        const auto dns_options = co_await dns.options(c, tenant, std::string("option-boundary-"), std::string("verified"));
        REQUIRE(c.query_count_ == before + 1 && dns_options.size() == static_cast<std::size_t>(end));
        const auto dns_page = co_await dns.list(c, tenant, 1, 10000, 0, std::string("option-boundary-"), std::string("verified"));
        REQUIRE(std::string(ruvia::to_json(dns_options)) == std::string(ruvia::to_json(dns_page.get<"list">())));
        before = c.query_count_;
        const auto certificate_options = co_await certificates.options(c, tenant, std::string("option-boundary-"), std::string_view("pending"), false);
        REQUIRE(c.query_count_ == before + 1 && certificate_options.size() == static_cast<std::size_t>(end));
        const auto certificate_page = co_await certificates.list(c, tenant, 1, 10000, 0, std::string("option-boundary-"), std::string_view("pending"), false, false, false);
        REQUIRE(std::string(ruvia::to_json(certificate_options)) == std::string(ruvia::to_json(certificate_page.get<"list">())));
    }
    (void)co_await tx.execute("ROLLBACK TO SAVEPOINT option_boundaries");
    (void)co_await tx.execute("RELEASE SAVEPOINT option_boundaries");
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
    co_await verify_provider_pages(c, tx);
    co_await verify_traffic(c, tx);
    co_await verify_overview_and_filters(c);
    co_await verify_details(c);
    co_await verify_online_window(c, tx);
    if (!service::geoip::get_xdb_database().available()) {
        co_await verify_dashboard_without_geoip(c);
    }
    co_await verify_dashboard_console_days(c, tx);
    co_await verify_dashboard_asns(c, tx);
    co_await verify_options(c, tx);
    co_await tx.rollback();
}

}  // namespace

int main(int argc, char** argv) {
    const bool with_geoip = argc == 2 && std::string_view{argv[1]} == "with-xdb";
    // The driver runs separate processes because the runtime database is configured once.
    service::config::geoip_configuration geoip;
    if (with_geoip) {
        const auto path = [](const char* name) {
            const auto* value = std::getenv(name);
            return std::filesystem::path(value ? value : "");
        };
        geoip.ipv4_path_ = path("FLEXEDGE_XDB_V4_PATH");
        geoip.ipv6_path_ = path("FLEXEDGE_XDB_V6_PATH");
    }
    service::geoip::configure_xdb_database(geoip);
    REQUIRE(service::geoip::get_xdb_database().available() == with_geoip);
    return test_support::run_postgres_test(verify);
}
