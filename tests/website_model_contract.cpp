#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/testing.h"

#include "service/domains/website/website_schema.h"

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

class website_validation_probe final {
public:
    RUVIA_CONTROLLER_GROUP("/website-validation-probe")
    RUVIA_ROUTES_BEGIN(website_validation_probe)
    RUVIA_POST("/", accept, ruvia::json_body<service::website::website_save_input>,
        service::website::website_config_validator);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> accept(ruvia::context& c) {
        const auto body = c.req().validated_json<service::website::website_save_input>();
        require(body.value().get<"status">().has_value(), "validatedJson wrapper missing");
        co_return c.text("accepted");
    }
};

constexpr std::string_view valid_body = R"json({
  "status":"enabled",
  "config":{
    "name":"site",
    "domains":[{"id":"11111111-1111-4111-8111-111111111111","hostname":"example.com","dns_mode":"managed"}],
    "origins":[{"id":"22222222-2222-4222-8222-222222222222","group":"main","protocol":"http","host":"127.0.0.1","port":80,"role":"primary","weight":1,"status":"enabled"}],
    "default_origin_group":"main","origin_host_header":"$host",
    "origin_connect_timeout_seconds":10,"origin_read_timeout_seconds":10,
    "pass_client_ip":false,"health_check_enabled":false,"health_check_path":"/health",
    "health_check_interval_seconds":30,"health_check_timeout_seconds":5,
    "health_check_expected_status":200,"healthy_threshold":2,"unhealthy_threshold":2,
    "access_log_enabled":false,"access_log_request_headers":false,"access_log_request_body":false,
    "access_log_response_headers":false,"access_log_query_params":false,"access_log_cookies":false,
    "access_log_referer":false,"access_log_user_agent":false,"access_log_status_code_ranges":["2xx"],
    "access_log_client_abort":false,"https_enabled":false,"certificate_ids":[],
    "minimum_tls_version":"1.2","force_https":false,"hsts_enabled":false,
    "response_compression_enabled":false,"response_compression_min_bytes":256,
    "response_compression_max_bytes":0,"response_compression_algorithms":["gzip"],
    "response_compression_mime_types":["text/html"],"response_compression_extensions":[".html"],
    "response_compression_excluded_extensions":[],"route_rules":[]
  }
})json";

}  // namespace

int main() {
    ruvia::test_app app;
    const auto accepted =
        app.request(ruvia::test_request::post("/website-validation-probe").json(valid_body));
    if (accepted.status() != ruvia::http_status::ok || accepted.body() != "accepted") {
        throw std::runtime_error(
            "valid request failed: " + std::to_string(accepted.status().value()) + " " +
            std::string(accepted.body()));
    }
    require(accepted.status() == ruvia::http_status::ok && accepted.body() == "accepted",
        "valid website JSON did not reach handler");

    std::string legacy_http2(valid_body);
    const auto force_https = legacy_http2.find("\"force_https\":false,");
    require(force_https != std::string::npos, "fixture force_https missing");
    legacy_http2.insert(force_https, "\"http2_enabled\":true,");
    const auto legacy =
        app.request(ruvia::test_request::post("/website-validation-probe").json(legacy_http2));
    require(legacy.status() == ruvia::http_status::ok && legacy.body() == "accepted",
        "removed http2_enabled key must be ignored as an unknown field");

    const auto missing_content_type =
        app.request(ruvia::test_request::post("/website-validation-probe").body(valid_body));
    require(missing_content_type.status() == ruvia::http_status::unsupported_media_type,
        "missing Content-Type was not rejected");
    const auto malformed =
        app.request(ruvia::test_request::post("/website-validation-probe").json("{"));
    require(malformed.status() == ruvia::http_status::bad_request,
        "malformed JSON was not rejected");

    const auto invalid =
        app.request(ruvia::test_request::post("/website-validation-probe")
                .json(R"({"status":"enabled","config":{"minimum_tls_version":"1.4"}})"));
    require(invalid.status() == ruvia::http_status::bad_request &&
                invalid.body().find("最低 TLS 版本不正确") != std::string_view::npos &&
                invalid.body().find("minimum_tls_version") != std::string_view::npos &&
                invalid.body().find("regex") != std::string_view::npos,
        "invalid TLS validation path/code/message contract changed");

    const auto null_required =
        app.request(ruvia::test_request::post("/website-validation-probe")
                .json(R"({"status":"enabled","config":{"domains":null}})"));
    require(null_required.status() == ruvia::http_status::bad_request &&
                null_required.body().find("config.domains") != std::string_view::npos &&
                null_required.body().find("invalid_type") != std::string_view::npos,
        "null field did not retain the model invalid_type path/code");
    const auto null_optional =
        app.request(ruvia::test_request::post("/website-validation-probe")
                .json(R"({"status":"enabled","config":{"name":null}})"));
    require(null_optional.status() == ruvia::http_status::bad_request &&
                null_optional.body().find("config.name") != std::string_view::npos &&
                null_optional.body().find("invalid_type") != std::string_view::npos,
        "optional transport nullability changed unexpectedly");
    const auto missing_status =
        app.request(ruvia::test_request::post("/website-validation-probe").json(R"({"config":{}})"));
    require(missing_status.status() == ruvia::http_status::bad_request,
        "missing required status was accepted");

    std::string duplicate_ids(valid_body);
    const auto domains_end = duplicate_ids.find("}],\n    \"origins\"");
    require(domains_end != std::string::npos, "fixture domain array missing");
    duplicate_ids.insert(domains_end + 1,
        ", "
        "{\"id\":\"11111111-1111-4111-8111-111111111111\",\"hostname\":\"other."
        "example.com\",\"dns_mode\":\"managed\"}");
    const auto duplicate =
        app.request(ruvia::test_request::post("/website-validation-probe").json(duplicate_ids));
    require(duplicate.status() == ruvia::http_status::bad_request,
        "duplicate domain/origin UUID was accepted");

    std::string cross_field(valid_body);
    const auto max_value = cross_field.find("\"response_compression_max_bytes\":0");
    require(max_value != std::string::npos, "fixture compression maximum missing");
    cross_field.replace(max_value, std::string_view("\"response_compression_max_bytes\":0").size(),
        "\"response_compression_max_bytes\":128");
    const auto mismatch =
        app.request(ruvia::test_request::post("/website-validation-probe").json(cross_field));
    require(mismatch.status() == ruvia::http_status::bad_request,
        "compression max/min cross-field mismatch was accepted");

    // Nodes reject a release whose route rule sets more headers than the protocol limit.
    const auto with_route_headers = [](std::size_t count) {
        std::string headers;
        for (std::size_t index = 0; index < count; ++index) {
            headers += (index == 0 ? "" : ",");
            headers += "{\"name\":\"X-Route-" + std::to_string(index) + "\",\"value\":\"v\"}";
        }
        std::string body(valid_body);
        const auto rules = body.find("\"route_rules\":[]");
        require(rules != std::string::npos, "fixture route rules missing");
        body.replace(rules, std::string_view("\"route_rules\":[]").size(),
            "\"route_rules\":[{\"id\":\"33333333-3333-4333-8333-333333333333\",\"status\":\"enabled\","
            "\"match_type\":\"prefix\",\"path\":\"/\",\"methods\":[],\"action\":\"proxy\","
            "\"rewrite_path\":\"\",\"redirect_url\":\"\",\"redirect_status\":0,\"origin_group\":\"main\","
            "\"request_headers\":[" +
                headers + "],\"response_headers\":[]}]");
        return body;
    };
    const auto at_limit = app.request(ruvia::test_request::post("/website-validation-probe")
            .json(with_route_headers(flexedge::node::max_route_headers)));
    require(at_limit.status() == ruvia::http_status::ok, "route headers at the protocol limit were rejected");
    const auto over_limit = app.request(ruvia::test_request::post("/website-validation-probe")
            .json(with_route_headers(flexedge::node::max_route_headers + 1)));
    require(over_limit.status() == ruvia::http_status::bad_request &&
                over_limit.body().find("config.route_rules[0].request_headers") != std::string_view::npos,
        "route headers over the protocol limit were accepted");

    std::string negative_maximum(valid_body);
    const auto zero_maximum = negative_maximum.find("\"response_compression_max_bytes\":0");
    require(zero_maximum != std::string::npos, "fixture max bytes value missing");
    negative_maximum.replace(zero_maximum,
        std::string_view("\"response_compression_max_bytes\":0").size(),
        "\"response_compression_max_bytes\":-1");
    const auto negative =
        app.request(ruvia::test_request::post("/website-validation-probe").json(negative_maximum));
    require(negative.status() == ruvia::http_status::bad_request &&
                negative.body().find("不能小于0") != std::string_view::npos &&
                negative.body().find("too_small") != std::string_view::npos,
        "negative compression size path/code/message changed");

    std::string no_domains(valid_body);
    const auto domain_array = no_domains.find("\"domains\":[{");
    const auto domain_end = no_domains.find("}],\n    \"origins\"", domain_array);
    require(domain_array != std::string::npos && domain_end != std::string::npos,
        "fixture domain object missing");
    no_domains.replace(domain_array, domain_end + 2 - domain_array, "\"domains\":[]");
    const auto empty_domains =
        app.request(ruvia::test_request::post("/website-validation-probe").json(no_domains));
    require(empty_domains.status() == ruvia::http_status::bad_request &&
                empty_domains.body().find("至少需要一个绑定域名") != std::string_view::npos,
        "empty domains min-rule message changed");
}
