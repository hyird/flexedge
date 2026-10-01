#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/web/App.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/Controller.h"
#include "ruvia/web/Testing.h"
#include "service/domains/website/website.schema.h"

namespace {

void require(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

class WebsiteValidationProbe final : public ruvia::Controller<WebsiteValidationProbe> {
  public:
    RUVIA_CONTROLLER_GROUP("/website-validation-probe")
    RUVIA_ROUTES_BEGIN
    RUVIA_POST("/", accept, ruvia::JsonBody<service::website::WebsiteSaveInput>,
               service::website::WebsiteConfigValidator);
    RUVIA_ROUTES_END

  private:
    ruvia::Task<ruvia::HttpResponse> accept(ruvia::Context& c) {
        const auto body = c.req().validatedJson<service::website::WebsiteSaveInput>();
        require(body.value().get<"status">().has_value(), "validatedJson wrapper missing");
        co_return c.text("accepted");
    }
};

constexpr std::string_view validBody = R"json({
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
    "minimum_tls_version":"1.2","force_https":false,"http2_enabled":false,"hsts_enabled":false,
    "response_compression_enabled":false,"response_compression_min_bytes":256,
    "response_compression_max_bytes":0,"response_compression_algorithms":["gzip"],
    "response_compression_mime_types":["text/html"],"response_compression_extensions":[".html"],
    "response_compression_excluded_extensions":[],"route_rules":[]
  }
})json";

} // namespace

int main() {
    ruvia::TestApp app;
    const auto accepted =
        app.request(ruvia::TestRequest::post("/website-validation-probe").json(validBody));
    if (accepted.status() != ruvia::http_status::kOk || accepted.body() != "accepted") {
        throw std::runtime_error(
            "valid request failed: " + std::to_string(accepted.status().value()) + " " +
            std::string(accepted.body()));
    }
    require(accepted.status() == ruvia::http_status::kOk && accepted.body() == "accepted",
            "valid website JSON did not reach handler");

    const auto missingContentType =
        app.request(ruvia::TestRequest::post("/website-validation-probe").body(validBody));
    require(missingContentType.status() == ruvia::http_status::kUnsupportedMediaType,
            "missing Content-Type was not rejected");
    const auto malformed =
        app.request(ruvia::TestRequest::post("/website-validation-probe").json("{"));
    require(malformed.status() == ruvia::http_status::kBadRequest,
            "malformed JSON was not rejected");

    const auto invalid =
        app.request(ruvia::TestRequest::post("/website-validation-probe")
                        .json(R"({"status":"enabled","config":{"minimum_tls_version":"1.4"}})"));
    require(invalid.status() == ruvia::http_status::kBadRequest &&
                invalid.body().find("最低 TLS 版本不正确") != std::string_view::npos &&
                invalid.body().find("minimum_tls_version") != std::string_view::npos &&
                invalid.body().find("regex") != std::string_view::npos,
            "invalid TLS validation path/code/message contract changed");

    const auto nullRequired =
        app.request(ruvia::TestRequest::post("/website-validation-probe")
                        .json(R"({"status":"enabled","config":{"domains":null}})"));
    require(nullRequired.status() == ruvia::http_status::kBadRequest &&
                nullRequired.body().find("config.domains") != std::string_view::npos &&
                nullRequired.body().find("invalid_type") != std::string_view::npos,
            "null field did not retain the model invalid_type path/code");
    const auto nullOptional =
        app.request(ruvia::TestRequest::post("/website-validation-probe")
                        .json(R"({"status":"enabled","config":{"name":null}})"));
    require(nullOptional.status() == ruvia::http_status::kBadRequest &&
                nullOptional.body().find("config.name") != std::string_view::npos &&
                nullOptional.body().find("invalid_type") != std::string_view::npos,
            "optional transport nullability changed unexpectedly");
    const auto missingStatus =
        app.request(ruvia::TestRequest::post("/website-validation-probe").json(R"({"config":{}})"));
    require(missingStatus.status() == ruvia::http_status::kBadRequest,
            "missing required status was accepted");

    std::string duplicateIds(validBody);
    const auto domainsEnd = duplicateIds.find("}],\n    \"origins\"");
    require(domainsEnd != std::string::npos, "fixture domain array missing");
    duplicateIds.insert(domainsEnd + 1,
                        ", "
                        "{\"id\":\"11111111-1111-4111-8111-111111111111\",\"hostname\":\"other."
                        "example.com\",\"dns_mode\":\"managed\"}");
    const auto duplicate =
        app.request(ruvia::TestRequest::post("/website-validation-probe").json(duplicateIds));
    require(duplicate.status() == ruvia::http_status::kBadRequest,
            "duplicate domain/origin UUID was accepted");

    std::string crossField(validBody);
    const auto maxValue = crossField.find("\"response_compression_max_bytes\":0");
    require(maxValue != std::string::npos, "fixture compression maximum missing");
    crossField.replace(maxValue, std::string_view("\"response_compression_max_bytes\":0").size(),
                       "\"response_compression_max_bytes\":128");
    const auto mismatch =
        app.request(ruvia::TestRequest::post("/website-validation-probe").json(crossField));
    require(mismatch.status() == ruvia::http_status::kBadRequest,
            "compression max/min cross-field mismatch was accepted");

    std::string negativeMaximum(validBody);
    const auto zeroMaximum = negativeMaximum.find("\"response_compression_max_bytes\":0");
    require(zeroMaximum != std::string::npos, "fixture max bytes value missing");
    negativeMaximum.replace(zeroMaximum,
                            std::string_view("\"response_compression_max_bytes\":0").size(),
                            "\"response_compression_max_bytes\":-1");
    const auto negative =
        app.request(ruvia::TestRequest::post("/website-validation-probe").json(negativeMaximum));
    require(negative.status() == ruvia::http_status::kBadRequest &&
                negative.body().find("不能小于0") != std::string_view::npos &&
                negative.body().find("too_small") != std::string_view::npos,
            "negative compression size path/code/message changed");

    std::string noDomains(validBody);
    const auto domainArray = noDomains.find("\"domains\":[{");
    const auto domainEnd = noDomains.find("}],\n    \"origins\"", domainArray);
    require(domainArray != std::string::npos && domainEnd != std::string::npos,
            "fixture domain object missing");
    noDomains.replace(domainArray, domainEnd + 2 - domainArray, "\"domains\":[]");
    const auto emptyDomains =
        app.request(ruvia::TestRequest::post("/website-validation-probe").json(noDomains));
    require(emptyDomains.status() == ruvia::http_status::kBadRequest &&
                emptyDomains.body().find("至少需要一个绑定域名") != std::string_view::npos,
            "empty domains min-rule message changed");
}
