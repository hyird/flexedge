#include <cstdio>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>

#include <ruvia/web/App.h>
#include <ruvia/web/Controller.h>
#include <ruvia/web/RequestValidation.h>
#include <ruvia/web/Testing.h>

#include "service/common/http.h"
#include "service/domains/certificate/certificate.schema.h"
#include "service/domains/cluster/cluster.schema.h"
#include "service/domains/dns_zone/dns_zone.schema.h"

namespace {

class ResourceContractController final : public ruvia::Controller<ResourceContractController> {
public:
    RUVIA_CONTROLLER_GROUP("/resource-contract")
    RUVIA_ROUTES_BEGIN
    RUVIA_POST("/dns-create", dnsCreate,
               ruvia::JsonBody<service::dns_zone::CreateDnsZoneBody>,
               service::dns_zone::CreateDnsZoneValidator);
    RUVIA_POST("/cluster", cluster,
               ruvia::JsonBody<service::cluster::SaveClusterBody>,
               service::cluster::SaveClusterValidator);
    RUVIA_POST("/certificate", certificate,
               ruvia::JsonBody<service::certificate::CreateCertificateBody>,
               service::certificate::CreateCertificateValidator);
    RUVIA_POST("/certificate-config", certificateConfig,
               ruvia::JsonBody<service::certificate_issuance::CertificateConfigInput>,
               service::certificate::CertificateConfigValidator);
    RUVIA_POST("/dns-config", dnsConfig,
               ruvia::JsonBody<service::dns_sync::ZoneConfigInput>,
               service::dns_zone::DnsZoneConfigValidator);
    RUVIA_ROUTES_END

private:
    ruvia::Task<ruvia::HttpResponse> dnsCreate(ruvia::Context& context) {
        (void)context.req().validatedJson<service::dns_zone::CreateDnsZoneBody>();
        co_return context.text("bound");
    }
    ruvia::Task<ruvia::HttpResponse> cluster(ruvia::Context& context) {
        (void)context.req().validatedJson<service::cluster::SaveClusterBody>();
        co_return context.text("bound");
    }
    ruvia::Task<ruvia::HttpResponse> certificate(ruvia::Context& context) {
        (void)context.req().validatedJson<service::certificate::CreateCertificateBody>();
        co_return context.text("bound");
    }
    ruvia::Task<ruvia::HttpResponse> certificateConfig(ruvia::Context& context) {
        (void)context.req().validatedJson<service::certificate_issuance::CertificateConfigInput>();
        co_return context.text("bound");
    }
    ruvia::Task<ruvia::HttpResponse> dnsConfig(ruvia::Context& context) {
        (void)context.req().validatedJson<service::dns_sync::ZoneConfigInput>();
        co_return context.text("bound");
    }
};

int failures = 0;

void expect(bool condition, std::string_view detail) {
    if (condition) return;
    ++failures;
    std::fprintf(stderr, "FAILED: %.*s\n", static_cast<int>(detail.size()), detail.data());
}

std::string validationIssues(const ruvia::HttpErrorInfo& error) {
    std::string result;
    for (const auto& issue : error.validationIssues()) {
        result.append(issue.field());
        result.push_back('|');
        result.append(issue.code());
        result.push_back('|');
        result.append(issue.message());
        result.push_back('\n');
    }
    return result;
}

void installErrorCapture(ruvia::TestApp& app) {
    app.onError([](ruvia::Context& context, ruvia::HttpErrorInfo error)
                    -> ruvia::Task<ruvia::HttpResponse> {
        context.status(error.status());
        const auto issues = validationIssues(error);
        std::pmr::string body(context.allocator<char>());
        body.append(service::common::responseErrorMessage(error));
        body.push_back('\n');
        body.append(issues);
        co_return context.text(std::move(body));
    });
}

void expectIssue(const ruvia::TestResponse& response, std::string_view field,
                 std::string_view code, std::string_view message) {
    const std::string expected = std::string(field) + "|" + std::string(code) + "|" +
                                 std::string(message) + "\n";
    const std::string prefix = std::string(message) + "\n" + expected;
    if (response.status() != ruvia::http_status::kBadRequest ||
        !response.body().starts_with(prefix)) {
        expect(false, prefix + "actual status/body=" +
                         std::to_string(response.status().value()) + "/" +
                         std::string(response.body()));
    }
}

void expectFirstIssue(const ruvia::TestResponse& response, std::string_view field,
                      std::string_view code, std::string_view message) {
    const std::string prefix = std::string(message) + "\n" + std::string(field) + "|" +
                               std::string(code) + "|" + std::string(message) + "\n";
    expect(response.status() == ruvia::http_status::kBadRequest &&
               response.body().starts_with(prefix),
           prefix);
}

void expectBound(const ruvia::TestResponse& response) {
    expect(response.status() == ruvia::http_status::kOk && response.body() == "bound",
           "request parsed, validated, bound and reached handler");
}

} // namespace

int main() {
    ruvia::TestApp app;
    installErrorCapture(app);

    expectBound(app.request(ruvia::TestRequest::post("/resource-contract/dns-create").json(
        R"({"dns_provider_id":"aaaaaaaa-0000-0000-0000-000000000001","domain":"example.com"})")));
    expectIssue(app.request(ruvia::TestRequest::post("/resource-contract/dns-create").json(
                     R"({"domain":"example.com"})")),
                "dns_provider_id", "required", "请选择 DNS 服务商账号");
    expectFirstIssue(app.request(ruvia::TestRequest::post("/resource-contract/dns-create").json(
                          R"({"dns_provider_id":null,"domain":"example.com"})")),
                     "dns_provider_id", "invalid_type", "must be a string");
    expectFirstIssue(app.request(ruvia::TestRequest::post("/resource-contract/dns-create").json(
                          R"({"dns_provider_id":"aaaaaaaa-0000-0000-0000-000000000001","domain":null})")),
                     "domain", "invalid_type", "must be a string");
    expectIssue(app.request(ruvia::TestRequest::post("/resource-contract/dns-create").json(
                     R"({"dns_provider_id":"bad","domain":"example.com"})")),
                "dns_provider_id", "regex", "DNS 服务商账号不正确");
    expectIssue(app.request(ruvia::TestRequest::post("/resource-contract/dns-create").json(
                     R"({"dns_provider_id":"aaaaaaaa-0000-0000-0000-000000000001","domain":""})")),
                "domain", "too_small", "域名不能为空");

    expectBound(app.request(ruvia::TestRequest::post("/resource-contract/cluster").json(
        R"({"name":"edge","dns_zone_id":"aaaaaaaa-0000-0000-0000-000000000001","hostname_prefix":"edge","status":"enabled"})")));
    expectIssue(app.request(ruvia::TestRequest::post("/resource-contract/cluster").json("{}")),
                "name", "required", "集群名称不能为空");
    expectFirstIssue(app.request(ruvia::TestRequest::post("/resource-contract/cluster").json(
                          R"({"name":null,"dns_zone_id":"aaaaaaaa-0000-0000-0000-000000000001","hostname_prefix":"edge","status":"enabled"})")),
                     "name", "invalid_type", "must be a string");
    expectIssue(app.request(ruvia::TestRequest::post("/resource-contract/cluster").json(
                     R"({"name":" ","dns_zone_id":"aaaaaaaa-0000-0000-0000-000000000001","hostname_prefix":"edge","status":"enabled"})")),
                "name", "regex", "集群名称不能为空");
    expectIssue(app.request(ruvia::TestRequest::post("/resource-contract/cluster").json(
                     R"({"name":"edge","dns_zone_id":"bad","hostname_prefix":"edge","status":"enabled"})")),
                "dns_zone_id", "regex", "托管域名不正确");
    expectIssue(app.request(ruvia::TestRequest::post("/resource-contract/cluster").json(
                     R"({"name":"edge","dns_zone_id":"aaaaaaaa-0000-0000-0000-000000000001","hostname_prefix":"-edge","status":"enabled"})")),
                "hostname_prefix", "regex", "主机前缀格式不正确");
    expectIssue(app.request(ruvia::TestRequest::post("/resource-contract/cluster").json(
                     R"({"name":"edge","dns_zone_id":"aaaaaaaa-0000-0000-0000-000000000001","hostname_prefix":"edge","status":"paused"})")),
                "status", "regex", "集群状态不正确");

    expectBound(app.request(ruvia::TestRequest::post("/resource-contract/certificate").json(
        R"({"domain":"example.com","certificate_provider_id":"aaaaaaaa-0000-0000-0000-000000000001","dns_zone_id":"aaaaaaaa-0000-0000-0000-000000000001","config":{"auto_renew":false}})")));
    expectIssue(app.request(ruvia::TestRequest::post("/resource-contract/certificate").json(
                     R"({"domain":"example.com","dns_zone_id":"aaaaaaaa-0000-0000-0000-000000000001","config":{"auto_renew":false}})")),
                "certificate_provider_id", "required", "请选择证书供应商");
    expectIssue(app.request(ruvia::TestRequest::post("/resource-contract/certificate").json(
                     R"({"domain":"*.invalid_domain","certificate_provider_id":"aaaaaaaa-0000-0000-0000-000000000001","dns_zone_id":"aaaaaaaa-0000-0000-0000-000000000001","config":{"auto_renew":false}})")),
                "domain", "regex", "域名格式不正确");
    expectFirstIssue(app.request(ruvia::TestRequest::post("/resource-contract/certificate").json(
                          R"({"domain":"example.com","certificate_provider_id":"aaaaaaaa-0000-0000-0000-000000000001","dns_zone_id":"aaaaaaaa-0000-0000-0000-000000000001","config":null})")),
                     "config", "invalid_type", "must be an object");
    expectIssue(app.request(ruvia::TestRequest::post("/resource-contract/certificate").json(
                     R"({"domain":"example.com","certificate_provider_id":"aaaaaaaa-0000-0000-0000-000000000001","dns_zone_id":"aaaaaaaa-0000-0000-0000-000000000001","config":{}})")),
                "config", "custom", "证书配置不正确");
    expectBound(app.request(ruvia::TestRequest::post("/resource-contract/certificate-config").json(
        R"({"auto_renew":false})")));
    expectIssue(app.request(ruvia::TestRequest::post("/resource-contract/certificate-config").json("{}")),
                "auto_renew", "required", "自动续签设置不能为空");
    expectFirstIssue(app.request(ruvia::TestRequest::post("/resource-contract/certificate-config").json(
                          R"({"auto_renew":null})")),
                     "auto_renew", "invalid_type", "must be a boolean");

    expectFirstIssue(app.request(ruvia::TestRequest::post("/resource-contract/dns-config").json(
                          R"({"records":null})")),
                     "records", "invalid_type", "must be an array");
    expectFirstIssue(app.request(ruvia::TestRequest::post("/resource-contract/dns-config").json(
                          R"({"records":[{"id":"aaaaaaaa-0000-0000-0000-000000000001","type":"A","name":"@","content":"192.0.2.1","ttl":null,"priority":0,"proxied":false,"line_code":"default"}]})")),
                     "records[0].ttl", "invalid_type", "must be a number");
    expectIssue(app.request(ruvia::TestRequest::post("/resource-contract/dns-config").json(
                     R"({"records":[{"id":"aaaaaaaa-0000-0000-0000-000000000001","type":"A","name":"@","content":"192.0.2.1","ttl":0,"proxied":false,"line_code":"default"}]})")),
                "records[0].ttl", "range", "TTL 不正确");
    expectIssue(app.request(ruvia::TestRequest::post("/resource-contract/dns-config").json(
                     R"({"records":[{"id":"aaaaaaaa-0000-0000-0000-000000000001","type":"A","name":"@","content":"192.0.2.1","ttl":1,"priority":65536,"proxied":false,"line_code":"default"}]})")),
                "records[0].priority", "range", "优先级不正确");
    expectIssue(app.request(ruvia::TestRequest::post("/resource-contract/dns-config").json(
                     R"({"records":[{"id":"aaaaaaaa-0000-0000-0000-000000000001","type":"A","name":"@","content":"192.0.2.1","ttl":1,"proxied":false,"line_code":"default"},{"id":"AAAAAAAA-0000-0000-0000-000000000001","type":"A","name":"@","content":"192.0.2.1","ttl":1,"proxied":false,"line_code":"default"}]})")),
                "records", "custom", "记录 ID 不能重复");
    expectBound(app.request(ruvia::TestRequest::post("/resource-contract/dns-config").json(
        R"({"records":[{"id":"aaaaaaaa-0000-0000-0000-000000000001","type":"A","name":"@","content":"192.0.2.1","ttl":1,"priority":0,"proxied":false,"line_code":"default"}]})")));
    std::string oversizedConfig = R"({"records":[)";
    for (std::size_t index = 0; index < 10001; ++index) {
        if (index != 0) oversizedConfig.push_back(',');
        oversizedConfig.append("{}");
    }
    oversizedConfig.append("]}");
    expectIssue(app.request(ruvia::TestRequest::post("/resource-contract/dns-config").json(
                     oversizedConfig)),
                "records", "too_big", "单个域名最多保存10000条记录");

    const auto wrongType = app.request(
        ruvia::TestRequest::post("/resource-contract/dns-create").body("{}", "text/plain"));
    expect(wrongType.status() == ruvia::http_status::kUnsupportedMediaType,
           "JsonBody rejects a non-JSON content type");
    const auto invalidJson = app.request(
        ruvia::TestRequest::post("/resource-contract/dns-create").json("{"));
    expect(invalidJson.status() == ruvia::http_status::kBadRequest,
           "JsonBody rejects malformed JSON");

    return failures == 0 ? 0 : 1;
}
