#include <ruvia/web/model_json.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "node/data/origin_selection.h"
#include "node/data/route_rules.h"
#include "service/common/domain_name.h"
#include "service/common/http.h"
#include "service/common/ip_address.h"
#include "service/config/outbound.h"
#include "service/config/schema.h"
#include "service/domains/overview/overview_types.h"
#include "service/features/attempt_trace/outbound_policy.h"
#include "service/features/certificate/worker.h"
#include "service/features/dns/record_reconciliation.h"
#include "service/features/dns/registry.h"
#include "service/features/dns_sync/worker.h"
#include "service/features/log_ingest/tail.h"
#include "service/features/node_config/mapper.h"
#include "service/features/node_dispatch/protocol.h"
#include "service/features/node_release/catalog.h"
#include "service/features/node_runtime/mapper.h"
#include "service/features/outbound_http/client.h"
#include "service/features/provider_verification/worker.h"
#include "service/features/sync_runtime/error.h"
#include "service/features/sync_runtime/state.h"
#include "service/features/website_config/validation.h"
#include "service/features/website_dispatch/worker.h"
#include "service/features/website_dns/mapper.h"
#include "service/features/website_dns/model.h"
#include "service/features/website_dns/runtime.h"
#include "service/utils/secret.h"
#include "service/utils/sensitive_string.h"

namespace {

template <typename function_type>
bool throws_runtime_error(function_type&& function) {
    try {
        function();
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

// Outbound HTTP of background attempts cannot bypass the attempt trace: the
// shared client takes the recorder as a required argument.
template <typename client_type>
concept untraced_outbound_send = requires(client_type& client, const ruvia::http_client_request_view& request) {
    service::outbound_http::send_buffered(client, request);
};
static_assert(!untraced_outbound_send<ruvia::http_client>);

}  // namespace

#define REQUIRE(condition)                                               \
    do {                                                                 \
        if (!(condition)) {                                              \
            std::cerr << "requirement failed: " #condition << '\n';      \
            throw std::runtime_error("requirement failed: " #condition); \
        }                                                                \
    } while (false)

int run_architecture_tests() {
    {
        const auto empty = service::website_dns::parse_stored("{}");
        REQUIRE(empty.has_value() && empty->domain_states_.empty());
        const auto runtime = service::website_dns::parse_stored(R"json({"domain_states":[
            {}, {"id":"domain", "resolution_status":"verified", "last_error":""}
        ]})json");
        REQUIRE(runtime.has_value() && runtime->domain_states_.size() == 2);
        REQUIRE(!runtime->domain_states_[0].id_.has_value());
        REQUIRE(!runtime->domain_states_[0].resolution_status_.has_value());
        REQUIRE(runtime->domain_states_[1].id_ == "domain");
        REQUIRE(runtime->domain_states_[1].resolution_status_ == "verified");
        REQUIRE(runtime->domain_states_[1].last_error_.has_value());
        REQUIRE(runtime->domain_states_[1].last_error_->empty());
        REQUIRE(!service::website_dns::parse_stored(R"({"domain_states":"invalid"})"));
        REQUIRE(ruvia::to_json(service::website_dns::to_output(*empty)) ==
                R"({"domain_states":[]})");
        REQUIRE(throws_runtime_error([&] { (void)service::website_dns::to_output(*runtime); }));
        service::website_dns::website_runtime_data complete;
        complete.domain_states_.push_back(runtime->domain_states_[1]);
        complete.domain_states_.push_back({.id_ = "invalid-domain",
            .resolution_status_ = "invalid",
            .last_verified_at_ = "2026-09-08T00:00:00Z",
            .last_error_ = "CNAME mismatch"});
        const auto serialized = ruvia::to_json(service::website_dns::to_output(complete));
        const auto restored = service::website_dns::parse_stored(serialized);
        REQUIRE(restored && restored->domain_states_.size() == 2);
        REQUIRE(restored->domain_states_[0].resolution_status_ == "verified");
        REQUIRE(restored->domain_states_[1].resolution_status_ == "invalid");
        REQUIRE(restored->domain_states_[1].last_error_ == "CNAME mismatch");
        REQUIRE(restored->domain_states_[1].last_verified_at_ == "2026-09-08T00:00:00Z");
    }
    {
        const auto empty = service::node_runtime::parse_stored("{}");
        REQUIRE(empty.has_value());
        REQUIRE(!empty->cpu_usage_.has_value());
        REQUIRE(!empty->connection_count_.has_value());
        REQUIRE(empty->origin_health_.empty());
        const auto runtime = service::node_runtime::parse_stored(R"json({
            "cpu_usage": 0, "connection_count": 0, "health": "healthy",
            "origin_health": [
                {"website_id":"web", "origin_id":"origin", "status":"healthy",
                 "checked_at_unix_millis":123, "latency_millis":0, "last_error":""},
                {"website_id":"incomplete"}
            ]
        })json");
        REQUIRE(runtime.has_value());
        REQUIRE(runtime->cpu_usage_.has_value() && *runtime->cpu_usage_ == 0);
        REQUIRE(runtime->connection_count_.has_value() && *runtime->connection_count_ == 0);
        REQUIRE(runtime->origin_health_.size() == 1);
        REQUIRE(runtime->origin_health_[0].website_id_ == "web");
        REQUIRE(runtime->origin_health_[0].checked_at_unix_millis_ == 123);
        REQUIRE(runtime->origin_health_[0].last_error_.has_value());
        REQUIRE(runtime->origin_health_[0].last_error_->empty());
        REQUIRE(!service::node_runtime::parse_stored(R"({"cpu_usage":"invalid"})"));
        REQUIRE(!service::node_runtime::parse_stored("{"));
    }
    {
        REQUIRE(!service::node_config::parse_stored("{}"));
        const auto empty = service::node_config::parse_stored(R"({"endpoints":[]})");
        REQUIRE(empty.has_value() && empty->endpoints_.empty());
        REQUIRE(!service::node_config::parse_stored(
            R"({"endpoints":[{"id":"one","ip_address":"192.0.2.1"}]})"));
        const auto config = service::node_config::parse_stored(
            R"({"endpoints":[{"id":"one","ip_address":"192.0.2.1","line_code":"default"}]})");
        REQUIRE(config.has_value() && config->endpoints_.size() == 1);
        const auto round_trip = service::node_config::parse_stored(
            ruvia::to_json(service::node_config::to_output(*config)));
        REQUIRE(round_trip.has_value() && round_trip->endpoints_.size() == 1);
        REQUIRE(round_trip->endpoints_[0].id_ == "one");
        REQUIRE(round_trip->endpoints_[0].ip_address_ == "192.0.2.1");
        REQUIRE(round_trip->endpoints_[0].line_code_ == "default");
    }
    {
        flexedge::node::v2::Website phased;
        phased.set_default_origin_group("default");
        auto add = [&](const char* action, const char* path) {
            auto* item = phased.add_route_rules();
            item->set_id(std::to_string(phased.route_rules_size()));
            item->set_enabled(true);
            item->set_action(action);
            item->set_match_type("prefix");
            item->set_path(path);
            return item;
        };
        auto* query = add("rewrite", "/old");
        query->set_query_mode("replace");
        query->set_query_string("v=2");
        auto* rewrite = add("rewrite", "/old");
        rewrite->set_rewrite_mode("replace_prefix");
        rewrite->set_rewrite_path("/api");
        auto* no_chain = add("rewrite", "/api");
        no_chain->set_rewrite_mode("replace_path");
        no_chain->set_rewrite_path("/wrong");
        auto* first = add("proxy", "/api");
        first->set_origin_group("first");
        auto* retained = first->add_request_headers();
        retained->set_name("X-Keep");
        retained->set_value("yes");
        auto* overwritten = first->add_request_headers();
        overwritten->set_name("X-Value");
        overwritten->set_value("old");
        auto* last = add("proxy", "/api");
        last->set_origin_group("last");
        auto* condition = last->add_conditions();
        condition->set_source("query");
        condition->set_name("v");
        condition->set_op("equals");
        condition->set_value("2");
        auto* final_header = last->add_request_headers();
        final_header->set_name("x-value");
        final_header->set_value("new");
        auto result = flexedge::node::evaluate_route_rules(phased, "GET", "/old/item?v=1");
        REQUIRE(result && result->origin_group() == "last");
        REQUIRE(flexedge::node::route_target("/old/item?v=1", &*result) == "/api/item?v=2");
        REQUIRE(result->request_headers_size() == 2);
        REQUIRE(result->request_headers(0).value() == "yes");
        REQUIRE(result->request_headers(1).value() == "new");
        last->set_enabled(false);
        REQUIRE(flexedge::node::evaluate_route_rules(phased, "GET", "/old/item")->origin_group() == "first");
        auto* redirect = add("redirect", "/old");
        redirect->set_redirect_url("/login#end");
        redirect->set_redirect_status(307);
        redirect->set_query_mode("drop");
        auto* later = add("redirect", "/old");
        later->set_redirect_url("/wrong");
        later->set_redirect_status(302);
        result = flexedge::node::evaluate_route_rules(phased, "GET", "/old/item?v=1");
        REQUIRE(result && result->action() == "redirect" && result->redirect_url() == "/login#end");
        REQUIRE(result->redirect_status() == 307);
        REQUIRE(flexedge::node::evaluate_route_rules(phased, "GET", "/missing")->origin_group() == "default");
        result = flexedge::node::evaluate_route_rules(phased, "GET", "/missing?");
        REQUIRE(result && flexedge::node::route_target("/missing?", &*result) == "/missing?");
    }
    using service::sync_runtime::marker_operation;
    using service::sync_runtime::marker_resource_type;
    REQUIRE(service::sync_runtime::resource_type_name(marker_resource_type::provider) == "provider");
    REQUIRE(service::sync_runtime::resource_type_name(marker_resource_type::dns_zone) == "dns_zone");
    REQUIRE(service::sync_runtime::resource_type_name(marker_resource_type::certificate) ==
            "certificate");
    REQUIRE(service::sync_runtime::resource_type_name(marker_resource_type::website) == "website");
    REQUIRE(service::sync_runtime::resource_column(marker_resource_type::provider) == "provider_id");
    REQUIRE(service::sync_runtime::resource_column(marker_resource_type::dns_zone) == "dns_zone_id");
    REQUIRE(service::sync_runtime::resource_column(marker_resource_type::certificate) ==
            "certificate_id");
    REQUIRE(service::sync_runtime::resource_column(marker_resource_type::website) == "website_id");
    REQUIRE(service::sync_runtime::marker_operation_name(marker_operation::verify) == "verify");
    REQUIRE(service::sync_runtime::marker_operation_name(marker_operation::sync) == "sync");
    REQUIRE(service::sync_runtime::marker_operation_name(marker_operation::sync_local) == "sync_local");
    REQUIRE(service::sync_runtime::marker_operation_name(marker_operation::sync_remote) ==
            "sync_remote");
    REQUIRE(service::sync_runtime::marker_operation_name(marker_operation::remove) == "delete");
    REQUIRE(service::sync_runtime::marker_operation_name(marker_operation::issue) == "issue");
    REQUIRE(service::sync_runtime::marker_operation_name(marker_operation::renew) == "renew");
    REQUIRE(service::sync_runtime::marker_operation_name(marker_operation::apply) == "apply");
    REQUIRE(service::sync_runtime::supports_marker_operation(marker_resource_type::dns_zone,
        marker_operation::sync_remote));
    REQUIRE(!service::sync_runtime::supports_marker_operation(marker_resource_type::website,
        marker_operation::remove));
    REQUIRE(throws_runtime_error([] {
        (void)service::sync_runtime::resource_type_name(static_cast<marker_resource_type>(99));
    }));
    REQUIRE(throws_runtime_error(
        [] { (void)service::sync_runtime::resource_column(static_cast<marker_resource_type>(99)); }));
    REQUIRE(
        throws_runtime_error([] { (void)service::sync_runtime::require_marker_operation("unknown"); }));
    const auto marker_started = std::chrono::steady_clock::now();
    const auto marker_lease = service::sync_runtime::make_running_lease("tenant", "marker", 7,
        "worker", marker_started);
    REQUIRE(marker_lease.marker_.tenant_id_ == "tenant");
    REQUIRE(marker_lease.marker_.marker_id_ == "marker");
    REQUIRE(marker_lease.marker_.version_ == 7);
    REQUIRE(marker_lease.owner_ == "worker");
    REQUIRE(marker_lease.started_ == marker_started);
    REQUIRE(throws_runtime_error([] {
        (void)service::sync_runtime::make_running_lease("tenant", "marker", 0, "worker",
            std::chrono::steady_clock::time_point{});
    }));

    static_assert(service::dns::dns_providers.size() == 2);
    static_assert(service::dns::find_dns_provider("cloudflare")->supports_proxy_);
    static_assert(service::dns::find_dns_provider("aliyun")->supports_routing_lines_);
    static_assert(service::dns::find_dns_provider("unknown") == nullptr);
    struct reconciliation_test_record final {
        std::string id_;
        bool desired_;
    };
    const std::vector<reconciliation_test_record> reconciliation_records{
        {"known", false},
        {"exact", true},
    };
    const auto known_record_plan = service::dns::plan_record_reconciliation(
        reconciliation_records, "known", [](const reconciliation_test_record&) { return false; },
        [](const reconciliation_test_record& record) { return record.desired_; },
        [] { throw std::runtime_error("unexpected duplicate"); });
    REQUIRE(known_record_plan.action_ == service::dns::record_reconciliation_action::update);
    REQUIRE(known_record_plan.record_->id_ == "known");
    const auto exact_record_plan = service::dns::plan_record_reconciliation(
        reconciliation_records, "missing",
        [](const reconciliation_test_record& record) { return record.id_ == "exact"; },
        [](const reconciliation_test_record& record) { return record.desired_; },
        [] { throw std::runtime_error("unexpected duplicate"); });
    REQUIRE(exact_record_plan.action_ == service::dns::record_reconciliation_action::reuse);
    REQUIRE(exact_record_plan.record_->id_ == "exact");
    const auto new_record_plan = service::dns::plan_record_reconciliation(
        reconciliation_records, "missing", [](const reconciliation_test_record&) { return false; },
        [](const reconciliation_test_record&) { return false; },
        [] { throw std::runtime_error("unexpected duplicate"); });
    REQUIRE(new_record_plan.action_ == service::dns::record_reconciliation_action::create);
    REQUIRE(new_record_plan.record_ == nullptr);
    REQUIRE(throws_runtime_error([&] {
        (void)service::dns::plan_record_reconciliation(
            reconciliation_records, "missing",
            [](const reconciliation_test_record& record) {
                return record.id_ == "exact" || record.id_ == "known";
            },
            [](const reconciliation_test_record&) { return false; },
            [] { throw std::runtime_error("duplicate record"); });
    }));
    REQUIRE(service::dns_sync::record_hostname("test", "a-z.xin") == "test.a-z.xin");
    REQUIRE(service::dns_sync::record_hostname("test.a-z.xin", "a-z.xin") == "test.a-z.xin");
    REQUIRE(service::dns_sync::record_hostname("@", "a-z.xin") == "a-z.xin");
    REQUIRE(service::dns_sync::is_traffic_record("CNAME"));
    REQUIRE(!service::dns_sync::is_traffic_record("TXT"));

    const auto configured_origins = service::config::make_outbound_origins(std::string{});
    REQUIRE(&service::config::outbound_origin_config(configured_origins,
                service::config::cloudflare_origin_alias) ==
            &configured_origins.cloudflare_);
    REQUIRE(&service::config::outbound_origin_config(configured_origins,
                service::config::aliyun_dns_origin_alias) ==
            &configured_origins.aliyun_dns_);

    const auto cursor = service::log_ingest::parse_tail_cursor(
        "1787776027394526:b2d6e77d-4a0c-4796-93d5-755d3c6d3837");
    REQUIRE(cursor);
    REQUIRE(service::log_ingest::encode_tail_cursor(cursor->ingested_unix_micros_, cursor->id_) ==
            "1787776027394526:b2d6e77d-4a0c-4796-93d5-755d3c6d3837");
    REQUIRE(!service::log_ingest::parse_tail_cursor("0:not-a-uuid"));
    REQUIRE(service::common::certificate_covers_hostname("*.example.com", "www.example.com"));
    REQUIRE(!service::common::certificate_covers_hostname("*.example.com", "example.com"));
    REQUIRE(service::common::certificate_covers_hostname("example.com", "example.com"));
    {
        service::node_dispatch::deployment_website_source source{};
        source.id_ = "website";
        service::website_config::website_config_data config{};
        config.https_enabled_ = true;
        for (const auto hostname : {"example.com", "www.example.com", "a.b.example.com"}) {
            service::website_config::website_domain_data domain{};
            domain.hostname_ = hostname;
            config.domains_.push_back(std::move(domain));
        }
        service::node_dispatch::cluster_deployment_source deployment{};
        service::node_dispatch::available_certificate certificate{};
        certificate.id_ = "certificate";
        certificate.domains_ = {"*.example.com"};
        deployment.certificates_by_website_[source.id_].push_back(certificate);
        // The artifact already exists; this test exercises name selection, not key decryption.
        std::unordered_map<std::string, std::string> digests{{certificate.id_, std::string(64, 'a')}};
        flexedge::node::v2::ClusterReleaseContent manifest;
        service::node_dispatch::cluster_release_artifact artifacts;
        flexedge::node::v2::Website website;
        REQUIRE(service::node_dispatch::append_website_domains(
            source, config, deployment, digests, manifest, artifacts, website));
        REQUIRE(!website.domains(0).https_enabled());
        REQUIRE(website.domains(1).https_enabled());
        REQUIRE(!website.domains(2).https_enabled());
        deployment.certificates_by_website_[source.id_][0].domains_.push_back("example.com");
        website.Clear();
        REQUIRE(service::node_dispatch::append_website_domains(
            source, config, deployment, digests, manifest, artifacts, website));
        REQUIRE(website.domains(0).https_enabled());
        REQUIRE(website.domains(0).certificate_digest() == std::string(64, 'a'));
        REQUIRE(!website.domains(2).https_enabled());
    }
    REQUIRE(!service::common::certificate_covers_hostname("*.example.com", "a.b.example.com"));
    REQUIRE(service::common::domain_belongs_to_zone("www.example.com", "example.com"));
    REQUIRE(service::common::is_hostname("*.example.com"));
    REQUIRE(!service::common::is_hostname("-invalid.example.com"));
    REQUIRE(service::common::parse_ip_address("2001:db8::1"));
    REQUIRE(!service::common::is_ipv4_address("2001:db8::1"));

    service::utils::sensitive_string secret(std::string("first-secret"));
    service::utils::sensitive_string moved(std::move(secret));
    REQUIRE(secret.view().empty());
    REQUIRE(moved.view() == "first-secret");
    service::utils::configure_secret_key(std::string(64, 'a'));
    const auto sealed = service::utils::seal_secret("sensitive-value");
    REQUIRE(sealed.starts_with("v1."));
    REQUIRE(service::utils::open_secret(sealed) == "sensitive-value");
    auto tampered = sealed;
    tampered.back() = tampered.back() == 'a' ? 'b' : 'a';
    REQUIRE(throws_runtime_error([&] { (void)service::utils::open_secret(tampered); }));

    service::overview::overview_resource_counts_dto resources;
    resources.set<"website_count">(2);
    resources.set<"domain_count">(3);
    resources.set<"certificate_count">(4);
    resources.set<"cluster_count">(1);
    resources.set<"node_count">(3);
    resources.set<"online_node_count">(2);
    service::overview::overview_issue_counts_dto issues;
    issues.set<"dns_zone_issue_count">(1);
    issues.set<"certificate_expiring_count">(2);
    issues.set<"certificate_failed_count">(0);
    issues.set<"active_marker_count">(3);
    issues.set<"retry_marker_count">(1);
    service::overview::overview_data_dto overview;
    overview.set<"resources">(std::move(resources));
    overview.set<"issues">(std::move(issues));
    (void)overview.ensure<"recent_markers">();
    REQUIRE(ruvia::to_json(overview).find("\"recent_markers\":[]") != std::string_view::npos);

    const auto& schema_migrations = service::config::schema_migrations;
    REQUIRE(schema_migrations.size() >= 20);
    REQUIRE(schema_migrations.front().id() == "0001");
    REQUIRE(schema_migrations.back().id() == "0024_task_attempt_traces");
    const auto remove_http2_migration = schema_migrations[schema_migrations.size() - 3].sql();
    REQUIRE(schema_migrations[schema_migrations.size() - 3].id() == "0022_remove_website_http2");
    REQUIRE(remove_http2_migration.find("sys_website") != std::string_view::npos);
    REQUIRE(remove_http2_migration.find("http2_enabled") != std::string_view::npos);
    REQUIRE(schema_migrations[schema_migrations.size() - 2].id() == "0023_task_attempt_details");
    const auto attempt_details_migration = schema_migrations[schema_migrations.size() - 2].sql();
    REQUIRE(attempt_details_migration.find("idx_sync_event_task_resource_version") != std::string_view::npos);
    REQUIRE(attempt_details_migration.find("'node'") != std::string_view::npos);
    REQUIRE(attempt_details_migration.find("row_number") != std::string_view::npos);
    const auto attempt_traces_migration = schema_migrations.back().sql();
    REQUIRE(attempt_traces_migration.find("sys_sync_event_trace") != std::string_view::npos);
    REQUIRE(attempt_traces_migration.find("ON DELETE CASCADE") != std::string_view::npos);
    // Every outbound operation has its own allow-list; credentials are never on one.
    for (const auto& policy : service::attempt_trace::outbound_policies) {
        REQUIRE(&service::attempt_trace::policy_for(policy.operation_) == &policy);
        for (const auto parameter : {std::string_view{"AccessKeyId"}, std::string_view{"Signature"},
                 std::string_view{"SignatureNonce"}, std::string_view{"access_key"}}) {
            REQUIRE(std::find(policy.query_.begin(), policy.query_.end(), parameter) == policy.query_.end());
        }
    }
    const auto& sync_event_migration = schema_migrations[19].sql();
    REQUIRE(sync_event_migration.starts_with("DO $flexedge_sync_result_events$"));
    REQUIRE(sync_event_migration.find("CREATE TABLE public.sys_sync_event") != std::string_view::npos);
    const auto& schema_baseline = schema_migrations.front().sql();
    // The production database records this checksum. Never fold later schema
    // work into migration 0001 after it has been released.
    REQUIRE(schema_baseline.find("CREATE TABLE public.sys_task") != std::string_view::npos);
    REQUIRE(schema_baseline.find("node_key_hash") != std::string_view::npos);
    REQUIRE(schema_baseline.find("device_key_fingerprint") != std::string_view::npos);
    REQUIRE(schema_baseline.find("sys_sync_event") == std::string_view::npos);
    REQUIRE(schema_baseline.find("DROP TABLE") == std::string_view::npos);
    REQUIRE(schema_baseline.find("migrate_data") == std::string_view::npos);
    std::string schema_sql;
    for (const auto& migration : schema_migrations) {
        schema_sql += migration.sql();
    }
    REQUIRE(schema_sql.find("RENAME COLUMN node_key_envelope TO node_secret_envelope") != std::string_view::npos);
    REQUIRE(schema_sql.find("RENAME COLUMN protocol_version TO schema_version") != std::string_view::npos);
    REQUIRE(schema_sql.find("CREATE TABLE public.sys_sync_task") != std::string_view::npos);
    REQUIRE(schema_sql.find("CREATE TABLE public.sys_sync_event") != std::string_view::npos);
    REQUIRE(schema_sql.find("uk_sync_task_resource") != std::string_view::npos);
    REQUIRE(schema_sql.find("idx_sync_event_tenant_id") != std::string_view::npos);
    REQUIRE(schema_sql.find("version bigint NOT NULL") != std::string_view::npos);
    REQUIRE(schema_sql.find("processed_version bigint DEFAULT 0 NOT NULL") != std::string_view::npos);
    REQUIRE(schema_sql.find("lease_until timestamptz") != std::string_view::npos);
    REQUIRE(schema_sql.find(
                "num_nonnulls(provider_id, dns_zone_id, certificate_id, website_id) = 1") != std::string_view::npos);
    REQUIRE(schema_sql.find("request_bytes bigint DEFAULT 0 NOT NULL") != std::string_view::npos);
    REQUIRE(schema_sql.find("idx_website_access_log_website_ingested") != std::string_view::npos);
    REQUIRE(schema_sql.find("idx_node_log_node_ingested") != std::string_view::npos);
    REQUIRE(service::sync_runtime::bounded_error(
                std::string(service::sync_runtime::max_error_message_length + 1, 'x'))
                .size() == service::sync_runtime::max_error_message_length);
    // 334 three-byte characters: the cut lands inside the last one and backs off.
    std::string multibyte_error;
    for (int index = 0; index < 334; ++index) {
        multibyte_error += "错";
    }
    REQUIRE(service::sync_runtime::bounded_error(multibyte_error).size() == 999);
    const auto invalid_provider_failure = service::provider_verification::detail::classify_task_failure(
        std::make_exception_ptr(std::invalid_argument("invalid provider config")));
    REQUIRE(invalid_provider_failure.permanent_);
    const auto retryable_provider_failure =
        service::provider_verification::detail::classify_task_failure(
            std::make_exception_ptr(service::certificate_issuance::certificate_provider_client_error(
                "upstream unavailable", true)));
    REQUIRE(!retryable_provider_failure.permanent_);

    flexedge::node::v2::Website route_website;
    route_website.set_default_origin_group("default");
    auto* fallback_route = route_website.add_route_rules();
    fallback_route->set_id("route-root");
    fallback_route->set_enabled(true);
    fallback_route->set_match_type("prefix");
    fallback_route->set_path("/");
    fallback_route->set_action("proxy");
    fallback_route->set_origin_group("default");
    auto* route = route_website.add_route_rules();
    route->set_id("route-1");
    route->set_enabled(true);
    route->set_match_type("prefix");
    route->set_path("/api");
    route->add_methods("GET");
    route->set_action("proxy");
    route->set_rewrite_path("/internal");
    route->set_origin_group("api");
    auto* default_origin = route_website.add_origins();
    default_origin->set_id("origin-default");
    default_origin->set_group("default");
    default_origin->set_protocol("http");
    default_origin->set_host("127.0.0.1");
    default_origin->set_port(80);
    default_origin->set_role("primary");
    default_origin->set_weight(100);
    default_origin->set_enabled(true);
    auto* route_origin = route_website.add_origins();
    route_origin->set_id("origin-route");
    route_origin->set_group("api");
    route_origin->set_protocol("http");
    route_origin->set_host("127.0.0.2");
    route_origin->set_port(80);
    route_origin->set_role("primary");
    route_origin->set_weight(100);
    route_origin->set_enabled(true);
    auto* request_header = route->add_request_headers();
    request_header->set_name("X-Route");
    request_header->set_value("enabled");
    auto* response_header = route->add_response_headers();
    response_header->set_name("X-Edge-Route");
    response_header->set_value("api");
    flexedge::node::validate_route_rules(route_website);
    // An earlier catch-all wins even over a longer path. Moving it below the
    // specific rule changes selection without changing any match fields.
    REQUIRE(flexedge::node::matched_route_rule(route_website, "GET", "/api/v1?q=1") == fallback_route);
    route_website.mutable_route_rules()->SwapElements(0, 1);
    const auto* matched = flexedge::node::matched_route_rule(route_website, "GET", "/api/v1?q=1");
    REQUIRE(matched == route);
    REQUIRE(flexedge::node::matched_route_rule(route_website, "GET", "/apix") == fallback_route);
    REQUIRE(flexedge::node::route_target("/api/v1?q=1", matched) == "/internal?q=1");
    const auto route_origins = flexedge::node::origin_candidates(route_website, 0, matched);
    REQUIRE(route_origins.size() == 1);
    REQUIRE(route_origins.front()->id() == "origin-route");
    const auto default_origins = flexedge::node::origin_candidates(route_website, 0);
    REQUIRE(default_origins.size() == 1);
    REQUIRE(default_origins.front()->id() == "origin-default");
    REQUIRE(flexedge::node::matched_route_rule(route_website, "POST", "/api/v1") == fallback_route);
    std::vector<std::pair<std::string, std::string>> route_headers{{"X-Route", "old"},
        {"Accept", "application/json"}};
    flexedge::node::apply_route_headers(route_headers, route->request_headers());
    REQUIRE(route_headers.size() == 2);
    REQUIRE(route_headers.front().first == "Accept");
    REQUIRE(route_headers.back().first == "X-Route");
    REQUIRE(route_headers.back().second == "enabled");
    // A hostname mismatch skips the earlier rule and continues down the list.
    route->add_hostnames("API.Example.com");
    REQUIRE(flexedge::node::matched_route_rule(route_website, "GET", "/api/v1", "api.example.com:443") == route);
    REQUIRE(flexedge::node::matched_route_rule(route_website, "GET", "/api/v1", "www.example.com") == fallback_route);
    REQUIRE(flexedge::node::matched_route_rule(route_website, "GET", "/apix", "api.example.com") == fallback_route);
    route->set_name("API rule");
    route->set_description("Route API traffic");
    route->set_rewrite_mode("replace_prefix");
    route->set_rewrite_path("/v2/");
    REQUIRE(flexedge::node::route_target("/api/users?a=1", route) == "/v2/users?a=1");
    REQUIRE(flexedge::node::route_target("/api?a=1", route) == "/v2/?a=1");
    route->set_query_mode("drop");
    REQUIRE(flexedge::node::route_target("/api/users?a=1", route) == "/v2/users");
    route->set_query_mode("replace");
    route->set_query_string("b=2&c=%2F");
    REQUIRE(flexedge::node::route_target("/api/users?a=1", route) == "/v2/users?b=2&c=%2F");
    route->set_rewrite_mode("strip_prefix");
    route->clear_rewrite_path();
    REQUIRE(flexedge::node::route_target("/api/users?a=1", route) == "/users?b=2&c=%2F");
    route->set_path("/api/");
    REQUIRE(flexedge::node::route_target("/api/users?a=1", route) == "/users?b=2&c=%2F");
    route->set_path("/api");
    route->clear_query_string();
    REQUIRE(flexedge::node::route_target("/api", route) == "/");
    flexedge::node::validate_route_rules(route_website);
    route->set_query_string("bad\r\nheader=1");
    REQUIRE(throws_runtime_error([&] { flexedge::node::validate_route_rules(route_website); }));
    route->clear_query_string();
    route->add_hostnames("api.example.com");
    REQUIRE(throws_runtime_error([&] { flexedge::node::validate_route_rules(route_website); }));
    route->clear_hostnames();
    route->set_action("redirect");
    route->clear_origin_group();
    route->set_rewrite_mode("none");
    route->set_redirect_status(302);
    route->set_redirect_url("https://example.com/new?target=1#part");
    route->set_query_mode("preserve");
    REQUIRE(flexedge::node::route_redirect_location("/old?source=1", *route) == "https://example.com/new?target=1#part");
    route->set_query_mode("drop");
    REQUIRE(flexedge::node::route_redirect_location("/old?source=1", *route) == "https://example.com/new#part");
    route->set_query_mode("replace");
    route->set_query_string("replacement=1");
    REQUIRE(flexedge::node::route_redirect_location("/old?source=1", *route) == "https://example.com/new?replacement=1#part");
    flexedge::node::validate_route_rules(route_website);

    const auto rule_input = ruvia::from_json<service::website_config::website_route_rule_input>(R"json({
      "id":"12345678-1234-4234-8234-123456789012","status":"enabled","match_type":"prefix",
      "path":"/api","methods":["GET"],"action":"proxy","rewrite_path":"/v2",
      "redirect_url":"","redirect_status":0,"origin_group":"default","request_headers":[],"response_headers":[],
      "name":"API","description":"API notes","hostnames":["API.Example.com"],
      "rewrite_mode":"replace_prefix","query_mode":"replace","query_string":"v=2"
    })json");
    REQUIRE(rule_input.has_value());
    const auto normalized_rule = service::website_config::normalize(*rule_input);
    REQUIRE(normalized_rule.has_value());
    REQUIRE(normalized_rule->hostnames_.front() == "api.example.com");
    REQUIRE(normalized_rule->name_ == "API");
    REQUIRE(normalized_rule->description_ == "API notes");
    REQUIRE(normalized_rule->rewrite_mode_ == "replace_prefix");
    REQUIRE(normalized_rule->query_mode_ == "replace");
    REQUIRE(normalized_rule->query_string_ == "v=2");
    service::website_config::website_config_data output_config{};
    output_config.route_rules_.push_back(*normalized_rule);
    const auto round_trip_json = ruvia::to_json(service::website_config::to_output(output_config));
    REQUIRE(round_trip_json.find("\"rewrite_mode\":\"replace_prefix\"") != std::string_view::npos);
    REQUIRE(round_trip_json.find("\"hostnames\":[\"api.example.com\"]") != std::string_view::npos);
    REQUIRE(round_trip_json.find("\"query_string\":\"v=2\"") != std::string_view::npos);

    const auto old_rule_input = ruvia::from_json<service::website_config::website_route_rule_input>(R"json({
      "id":"12345678-1234-4234-8234-123456789012","status":"enabled","match_type":"prefix",
      "path":"/api","methods":[],"action":"proxy","rewrite_path":"/internal",
      "redirect_url":"","redirect_status":0,"origin_group":"default","request_headers":[],"response_headers":[]
    })json");
    REQUIRE(old_rule_input.has_value());
    const auto old_rule = service::website_config::normalize(*old_rule_input);
    REQUIRE(old_rule.has_value());
    REQUIRE(old_rule->hostnames_.empty());
    REQUIRE(old_rule->rewrite_mode_ == "replace_path");
    REQUIRE(old_rule->query_mode_ == "preserve");
    REQUIRE(old_rule->conditions_.empty());
    REQUIRE(round_trip_json.find("\"conditions\":[]") != std::string_view::npos);
    {
        namespace policy = flexedge::route_policy;
        auto pattern = policy::compile_pattern("^/old/([^/]+)/(.*)$");
        REQUIRE(pattern != nullptr);
        REQUIRE(policy::expand_captures("/new/${2}/${1}?cash=$$", "/old/books/a%2Fb", *pattern) == "/new/a%2Fb/books?cash=$");
        REQUIRE(!policy::capture_template("/${3}", 2));
        REQUIRE(!policy::capture_template("/${10}", 2));
        REQUIRE(!policy::capture_template("/${broken}", 2));
        REQUIRE(policy::capture_template("/$${1}", 0));
        REQUIRE(!policy::compile_pattern("(?<=a)b"));
        REQUIRE(!policy::compile_pattern("(a)\\1"));
        REQUIRE(!policy::compile_pattern("["));
        REQUIRE(!policy::compile_pattern(std::string(513, 'a')));
        REQUIRE(!policy::compile_pattern("(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)"));
        auto optional = policy::compile_pattern("^/a(?:/(.*))?$");
        REQUIRE(policy::expand_captures("/b/${1}", "/a", *optional) == "/b/");
        auto bounded = policy::compile_pattern("(a+)+$");
        REQUIRE(bounded != nullptr);
        REQUIRE(!RE2::PartialMatch(std::string(100000, 'a') + "!", *bounded));
        const auto query = policy::get_query_values("/?tag=one&tag=two&space=a+b&escaped=%2B&flag");
        policy::header_values headers{{"X-Channel", " beta "}, {"x-channel", "stable"}};
        REQUIRE(policy::condition_matches("header", "x-channel", "equals", "beta", headers, query));
        REQUIRE(!policy::condition_matches("header", "x-channel", "not_equals", "beta", headers, query));
        REQUIRE(policy::condition_matches("query", "tag", "equals", "two", headers, query));
        REQUIRE(!policy::condition_matches("query", "tag", "not_equals", "one", headers, query));
        REQUIRE(policy::condition_matches("query", "space", "equals", "a b", headers, query));
        REQUIRE(policy::condition_matches("query", "escaped", "equals", "+", headers, query));
        REQUIRE(policy::condition_matches("query", "flag", "equals", "", headers, query));
        REQUIRE(policy::condition_matches("query", "missing", "absent", "", headers, query));
        REQUIRE(!policy::condition_matches("query", "missing", "not_equals", "x", headers, query));
        REQUIRE(!policy::get_query_values("/?tag=%FF"));
        REQUIRE(!policy::get_query_values("/?tag=%G1"));
        REQUIRE(!policy::condition_matches("query", "missing", "absent", "", headers, policy::get_query_values("/?tag=%")));
        REQUIRE(!policy::condition_options("header", "X Bad", "equals", "yes"));
        REQUIRE(!policy::condition_options("query", "q", "exists", "nonempty"));
        flexedge::node::v2::Website rich;
        const auto add = [&](std::string_view id, std::string_view type, std::string_view path) {
            auto* item = rich.add_route_rules();
            item->set_id(std::string(id));
            item->set_enabled(true);
            item->set_match_type(std::string(type));
            item->set_path(std::string(path));
            item->set_action("redirect");
            item->set_redirect_url("/new");
            item->set_redirect_status(307);
            return item;
        };
        auto* regex = add("regex", "regex", "^/old/(.*)$");
        regex->set_redirect_url("/new/${1}#section");
        regex->set_query_mode("replace");
        regex->set_query_string("v=2");
        auto* condition = regex->add_conditions();
        condition->set_source("header");
        condition->set_name("X-Channel");
        condition->set_op("equals");
        condition->set_value("beta");
        auto* second = regex->add_conditions();
        second->set_source("query");
        second->set_name("tag");
        second->set_op("equals");
        second->set_value("two");
        auto* regex_later = add("regex-later", "regex", "^/old/([^/]+)(/.*)?$");
        auto* suffix = add("suffix", "suffix", ".jpg");
        auto* prefix = add("prefix", "prefix", "/old/api");
        auto* exact = add("exact", "exact", "/old/api/a.jpg");
        flexedge::node::route_patterns compiled;
        flexedge::node::validate_route_rules(rich, &compiled);
        REQUIRE(compiled.size() == 2);
        REQUIRE(flexedge::node::matched_route_rule(rich, "GET", "/old/a?tag=two", "", headers, &compiled) == regex);
        REQUIRE(flexedge::node::matched_route_rule(rich, "GET", "/old/a?tag=one", "", headers, &compiled) == regex_later);
        REQUIRE(flexedge::node::matched_route_rule(rich, "GET", "/old/a?tag=two", "", {}, &compiled) == regex_later);
        REQUIRE(flexedge::node::matched_route_rule(rich, "GET", "/old/a.jpg", "", {}, &compiled) == regex_later);
        REQUIRE(flexedge::node::matched_route_rule(rich, "GET", "/old/api/b.jpg", "", {}, &compiled) == regex_later);
        REQUIRE(flexedge::node::matched_route_rule(rich, "GET", "/old/api/a.jpg", "", {}, &compiled) == regex_later);
        regex_later->set_enabled(false);
        REQUIRE(flexedge::node::matched_route_rule(rich, "GET", "/old/api/a.jpg", "", {}, &compiled) == suffix);
        suffix->set_enabled(false);
        REQUIRE(flexedge::node::matched_route_rule(rich, "GET", "/old/api/a.jpg", "", {}, &compiled) == prefix);
        rich.mutable_route_rules()->SwapElements(3, 4);
        REQUIRE(flexedge::node::matched_route_rule(rich, "GET", "/old/api/a.jpg", "", {}, &compiled) == exact);
        REQUIRE(flexedge::node::route_redirect_location("/old/a%2Fb?tag=two", *regex, &compiled) == "/new/a%2Fb?v=2#section");
        regex->set_redirect_status(308);
        flexedge::node::validate_route_rules(rich);
        regex->set_redirect_status(303);
        REQUIRE(throws_runtime_error([&] { flexedge::node::validate_route_rules(rich); }));

        auto input = ruvia::from_json<service::website_config::website_route_rule_input>(R"json({
          "id":"12345678-1234-4234-8234-123456789012","status":"enabled","match_type":"regex",
          "path":"^/old/(.*)$","methods":["POST"],"action":"redirect","rewrite_path":"",
          "redirect_url":"/new/${1}","redirect_status":308,"origin_group":"","request_headers":[],"response_headers":[],
          "conditions":[{"source":"query","name":"tag","op":"equals","value":"two"}]
        })json");
        REQUIRE(input.has_value());
        struct validation_errors {
            std::vector<std::string> fields_;
            void add(std::string field, std::string_view, std::string_view) {
                fields_.push_back(std::move(field));
            }
        } validation;
        service::website_config::website_route_rule_validator{}.validate_nested(*input, "route", validation);
        REQUIRE(validation.fields_.empty());
        const auto normalized = service::website_config::normalize(*input);
        REQUIRE(normalized && normalized->conditions_.size() == 1);
        output_config.route_rules_ = {*normalized};
        const auto json = ruvia::to_json(service::website_config::to_output(output_config));
        REQUIRE(json.find("\"conditions\":[{\"source\":\"query\"") != std::string_view::npos);
        input->set<"redirect_url">("/new/${2}");
        service::website_config::website_route_rule_validator{}.validate_nested(*input, "route", validation);
        REQUIRE(!validation.fields_.empty());
        validation.fields_.clear();
        input->set<"action">("rewrite");
        input->set<"redirect_url">("");
        input->set<"redirect_status">(0);
        input->set<"rewrite_mode">("replace_path");
        input->set<"rewrite_path">("/api/${1}");
        service::website_config::website_route_rule_validator{}.validate_nested(*input, "route", validation);
        REQUIRE(validation.fields_.empty());
        REQUIRE(service::website_config::normalize(*input)->action_ == "rewrite");
        input->set<"origin_group">("default");
        service::website_config::website_route_rule_validator{}.validate_nested(*input, "route", validation);
        REQUIRE(!validation.fields_.empty());
    }
    auto* invalid_route = route_website.add_route_rules();
    invalid_route->set_id("route-2");
    invalid_route->set_match_type("exact");
    invalid_route->set_path("/");
    invalid_route->set_action("proxy");
    auto* invalid_header = invalid_route->add_request_headers();
    invalid_header->set_name("Content-Length");
    invalid_header->set_value("0");
    REQUIRE(throws_runtime_error([&] { flexedge::node::validate_route_rules(route_website); }));
    return 0;
}

int main() {
    try {
        return run_architecture_tests();
    } catch (const std::exception& error) {
        std::cerr << "architecture test failed: " << error.what() << '\n';
        return 1;
    }
}