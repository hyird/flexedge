#include <ruvia/web/app.h>
#include <ruvia/web/controller.h>
#include <ruvia/web/request_validation.h>
#include <ruvia/web/testing.h>

#include <cstdio>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>

#include "service/common/http.h"
#include "service/domains/certificate/certificate_schema.h"
#include "service/domains/cluster/cluster_schema.h"
#include "service/domains/dns_zone/dns_zone_schema.h"

namespace {

class resource_contract_controller final {
public:
    RUVIA_CONTROLLER_GROUP("/resource-contract")
    RUVIA_ROUTES_BEGIN(resource_contract_controller)
    RUVIA_POST("/dns-create", dns_create,
        ruvia::json_body<service::dns_zone::create_dns_zone_body>,
        service::dns_zone::create_dns_zone_validator);
    RUVIA_POST("/cluster", cluster,
        ruvia::json_body<service::cluster::save_cluster_body>,
        service::cluster::save_cluster_validator);
    RUVIA_POST("/certificate", certificate,
        ruvia::json_body<service::certificate::create_certificate_body>,
        service::certificate::create_certificate_validator);
    RUVIA_POST("/certificate-config", certificate_config,
        ruvia::json_body<service::certificate_issuance::certificate_config_input>,
        service::certificate::certificate_config_validator);
    RUVIA_POST("/dns-config", dns_config, service::dns_zone::dns_zone_config_body);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> dns_create(ruvia::context& context) {
        (void)context.req().validated_json<service::dns_zone::create_dns_zone_body>();
        co_return context.text("bound");
    }
    ruvia::task<ruvia::http_response> cluster(ruvia::context& context) {
        (void)context.req().validated_json<service::cluster::save_cluster_body>();
        co_return context.text("bound");
    }
    ruvia::task<ruvia::http_response> certificate(ruvia::context& context) {
        (void)context.req().validated_json<service::certificate::create_certificate_body>();
        co_return context.text("bound");
    }
    ruvia::task<ruvia::http_response> certificate_config(ruvia::context& context) {
        (void)context.req().validated_json<service::certificate_issuance::certificate_config_input>();
        co_return context.text("bound");
    }
    ruvia::task<ruvia::http_response> dns_config(ruvia::context& context) {
        (void)context.request_state<service::dns_sync::zone_config_input>();
        co_return context.text("bound");
    }
};

int failures = 0;

void expect(bool condition, std::string_view detail) {
    if (condition) {
        return;
    }
    ++failures;
    std::fprintf(stderr, "FAILED: %.*s\n", static_cast<int>(detail.size()), detail.data());
}

std::string validation_issues(const ruvia::http_error_info& error) {
    std::string result;
    for (const auto& issue : error.validation_issues()) {
        result.append(issue.field());
        result.push_back('|');
        result.append(issue.code());
        result.push_back('|');
        result.append(issue.message());
        result.push_back('\n');
    }
    return result;
}

void install_error_capture(ruvia::test_app& app) {
    app.on_error([](ruvia::context& context, ruvia::http_error_info error)
                     -> ruvia::task<ruvia::http_response> {
        context.status(error.status());
        const auto issues = validation_issues(error);
        std::pmr::string body(context.allocator<char>());
        body.append(service::common::response_error_message(error));
        body.push_back('\n');
        body.append(issues);
        co_return context.text(std::move(body));
    });
}

void expect_issue(const ruvia::test_response& response, std::string_view field,
    std::string_view code, std::string_view message) {
    const std::string expected = std::string(field) + "|" + std::string(code) + "|" +
                                 std::string(message) + "\n";
    const std::string prefix = std::string(message) + "\n" + expected;
    if (response.status() != ruvia::http_status::bad_request ||
        !response.body().starts_with(prefix)) {
        expect(false, prefix + "actual status/body=" +
                          std::to_string(response.status().value()) + "/" +
                          std::string(response.body()));
    }
}

void expect_first_issue(const ruvia::test_response& response, std::string_view field,
    std::string_view code, std::string_view message) {
    const std::string prefix = std::string(message) + "\n" + std::string(field) + "|" +
                               std::string(code) + "|" + std::string(message) + "\n";
    expect(response.status() == ruvia::http_status::bad_request &&
               response.body().starts_with(prefix),
        prefix);
}

void expect_bound(const ruvia::test_response& response) {
    expect(response.status() == ruvia::http_status::ok && response.body() == "bound",
        "request parsed, validated, bound and reached handler");
}

}  // namespace

int main() {
    ruvia::test_app app;
    install_error_capture(app);

    expect_bound(app.request(ruvia::test_request::post("/resource-contract/dns-create").json(R"({"dns_provider_id":"aaaaaaaa-0000-0000-0000-000000000001","domain":"example.com"})")));
    expect_issue(app.request(ruvia::test_request::post("/resource-contract/dns-create").json(R"({"domain":"example.com"})")),
        "dns_provider_id", "required", "请选择 DNS 服务商账号");
    expect_first_issue(app.request(ruvia::test_request::post("/resource-contract/dns-create").json(R"({"dns_provider_id":null,"domain":"example.com"})")),
        "dns_provider_id", "invalid_type", "must be a string");
    expect_first_issue(app.request(ruvia::test_request::post("/resource-contract/dns-create").json(R"({"dns_provider_id":"aaaaaaaa-0000-0000-0000-000000000001","domain":null})")),
        "domain", "invalid_type", "must be a string");
    expect_issue(app.request(ruvia::test_request::post("/resource-contract/dns-create").json(R"({"dns_provider_id":"bad","domain":"example.com"})")),
        "dns_provider_id", "regex", "DNS 服务商账号不正确");
    expect_issue(app.request(ruvia::test_request::post("/resource-contract/dns-create").json(R"({"dns_provider_id":"aaaaaaaa-0000-0000-0000-000000000001","domain":""})")),
        "domain", "too_small", "域名不能为空");

    expect_bound(app.request(ruvia::test_request::post("/resource-contract/cluster").json(R"({"name":"edge","dns_zone_id":"aaaaaaaa-0000-0000-0000-000000000001","hostname_prefix":"edge","status":"enabled"})")));
    expect_issue(app.request(ruvia::test_request::post("/resource-contract/cluster").json("{}")),
        "name", "required", "集群名称不能为空");
    expect_first_issue(app.request(ruvia::test_request::post("/resource-contract/cluster").json(R"({"name":null,"dns_zone_id":"aaaaaaaa-0000-0000-0000-000000000001","hostname_prefix":"edge","status":"enabled"})")),
        "name", "invalid_type", "must be a string");
    expect_issue(app.request(ruvia::test_request::post("/resource-contract/cluster").json(R"({"name":" ","dns_zone_id":"aaaaaaaa-0000-0000-0000-000000000001","hostname_prefix":"edge","status":"enabled"})")),
        "name", "regex", "集群名称不能为空");
    expect_issue(app.request(ruvia::test_request::post("/resource-contract/cluster").json(R"({"name":"edge","dns_zone_id":"bad","hostname_prefix":"edge","status":"enabled"})")),
        "dns_zone_id", "regex", "托管域名不正确");
    expect_issue(app.request(ruvia::test_request::post("/resource-contract/cluster").json(R"({"name":"edge","dns_zone_id":"aaaaaaaa-0000-0000-0000-000000000001","hostname_prefix":"-edge","status":"enabled"})")),
        "hostname_prefix", "regex", "主机前缀格式不正确");
    expect_issue(app.request(ruvia::test_request::post("/resource-contract/cluster").json(R"({"name":"edge","dns_zone_id":"aaaaaaaa-0000-0000-0000-000000000001","hostname_prefix":"edge","status":"paused"})")),
        "status", "regex", "集群状态不正确");

    expect_bound(app.request(ruvia::test_request::post("/resource-contract/certificate").json(R"({"domain":"example.com","certificate_provider_id":"aaaaaaaa-0000-0000-0000-000000000001","dns_zone_id":"aaaaaaaa-0000-0000-0000-000000000001","config":{"auto_renew":false}})")));
    expect_issue(app.request(ruvia::test_request::post("/resource-contract/certificate").json(R"({"domain":"example.com","dns_zone_id":"aaaaaaaa-0000-0000-0000-000000000001","config":{"auto_renew":false}})")),
        "certificate_provider_id", "required", "请选择证书供应商");
    expect_issue(app.request(ruvia::test_request::post("/resource-contract/certificate").json(R"({"domain":"*.invalid_domain","certificate_provider_id":"aaaaaaaa-0000-0000-0000-000000000001","dns_zone_id":"aaaaaaaa-0000-0000-0000-000000000001","config":{"auto_renew":false}})")),
        "domain", "regex", "域名格式不正确");
    expect_first_issue(app.request(ruvia::test_request::post("/resource-contract/certificate").json(R"({"domain":"example.com","certificate_provider_id":"aaaaaaaa-0000-0000-0000-000000000001","dns_zone_id":"aaaaaaaa-0000-0000-0000-000000000001","config":null})")),
        "config", "invalid_type", "must be an object");
    expect_issue(app.request(ruvia::test_request::post("/resource-contract/certificate").json(R"({"domain":"example.com","certificate_provider_id":"aaaaaaaa-0000-0000-0000-000000000001","dns_zone_id":"aaaaaaaa-0000-0000-0000-000000000001","config":{}})")),
        "config", "custom", "证书配置不正确");
    expect_bound(app.request(ruvia::test_request::post("/resource-contract/certificate-config").json(R"({"auto_renew":false})")));
    expect_issue(app.request(ruvia::test_request::post("/resource-contract/certificate-config").json("{}")),
        "auto_renew", "required", "自动续签设置不能为空");
    expect_first_issue(app.request(ruvia::test_request::post("/resource-contract/certificate-config").json(R"({"auto_renew":null})")),
        "auto_renew", "invalid_type", "must be a boolean");

    expect_first_issue(app.request(ruvia::test_request::post("/resource-contract/dns-config").json(R"({"records":null})")),
        "records", "invalid_type", "must be an array");
    expect_first_issue(app.request(ruvia::test_request::post("/resource-contract/dns-config").json(R"({"records":[{"id":"aaaaaaaa-0000-0000-0000-000000000001","type":"A","name":"@","content":"192.0.2.1","ttl":null,"priority":0,"proxied":false,"line_code":"default"}]})")),
        "records[0].ttl", "invalid_type", "must be a number");
    expect_issue(app.request(ruvia::test_request::post("/resource-contract/dns-config").json(R"({"records":[{"id":"aaaaaaaa-0000-0000-0000-000000000001","type":"A","name":"@","content":"192.0.2.1","ttl":0,"proxied":false,"line_code":"default"}]})")),
        "records[0].ttl", "range", "TTL 不正确");
    expect_issue(app.request(ruvia::test_request::post("/resource-contract/dns-config").json(R"({"records":[{"id":"aaaaaaaa-0000-0000-0000-000000000001","type":"A","name":"@","content":"192.0.2.1","ttl":1,"priority":65536,"proxied":false,"line_code":"default"}]})")),
        "records[0].priority", "range", "优先级不正确");
    expect_issue(app.request(ruvia::test_request::post("/resource-contract/dns-config").json(R"({"records":[{"id":"aaaaaaaa-0000-0000-0000-000000000001","type":"A","name":"@","content":"192.0.2.1","ttl":1,"proxied":false,"line_code":"default"},{"id":"AAAAAAAA-0000-0000-0000-000000000001","type":"A","name":"@","content":"192.0.2.1","ttl":1,"proxied":false,"line_code":"default"}]})")),
        "records", "custom", "记录 ID 不能重复");
    expect_bound(app.request(ruvia::test_request::post("/resource-contract/dns-config").json(R"({"records":[{"id":"aaaaaaaa-0000-0000-0000-000000000001","type":"A","name":"@","content":"192.0.2.1","ttl":1,"priority":0,"proxied":false,"line_code":"default"}]})")));
    std::string oversized_config = R"({"records":[)";
    for (std::size_t index = 0; index < 10001; ++index) {
        if (index != 0) {
            oversized_config.push_back(',');
        }
        oversized_config.append("{}");
    }
    oversized_config.append("]}");
    expect_issue(app.request(ruvia::test_request::post("/resource-contract/dns-config").json(oversized_config)),
        "records", "too_big", "单个域名最多保存10000条记录");

    const auto wrong_type = app.request(
        ruvia::test_request::post("/resource-contract/dns-create").body("{}", "text/plain"));
    expect(wrong_type.status() == ruvia::http_status::unsupported_media_type,
        "JsonBody rejects a non-JSON content type");
    const auto invalid_json = app.request(
        ruvia::test_request::post("/resource-contract/dns-create").json("{"));
    expect(invalid_json.status() == ruvia::http_status::bad_request,
        "JsonBody rejects malformed JSON");

    return failures == 0 ? 0 : 1;
}
