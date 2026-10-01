#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#include <ruvia/web/Controller.h>
#include <ruvia/web/Testing.h>

#include "service/domains/node/node.types.h"

namespace {

class NodeModelContractController final : public ruvia::Controller<NodeModelContractController> {
  public:
    RUVIA_CONTROLLER_GROUP("/node-model-contract")
    RUVIA_ROUTES_BEGIN
    RUVIA_POST("/", validate, ruvia::JsonBody<service::node::NodeSaveInput>);
    RUVIA_ROUTES_END

  private:
    ruvia::Task<ruvia::HttpResponse> validate(ruvia::Context& context) {
        const auto body = context.req().validatedJson<service::node::NodeSaveInput>();
        ruvia::Validator validator({.resource = context.arena()});
        service::node::validateNodeSaveInput(body.value(), validator);
        std::move(validator).throwIfInvalid();
        co_return context.text(body.raw());
    }
};

void expect(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void expectIssue(ruvia::TestApp& app, std::string_view json, std::string_view path,
                 std::string_view code, std::string_view message,
                 std::string_view firstMessage = {}) {
    const auto response = app.request(
        ruvia::TestRequest::post("/node-model-contract").json(json));
    expect(response.status() == ruvia::http_status::kBadRequest, "invalid node input was accepted");
    const auto firstMessageKey = response.body().find(R"("message":")");
    const auto firstMessageStart = firstMessageKey == std::string_view::npos
                                       ? firstMessageKey
                                       : firstMessageKey + std::string_view(R"("message":")").size();
    const auto firstMessageEnd = firstMessageStart == std::string_view::npos
                                     ? firstMessageStart
                                     : response.body().find('"', firstMessageStart);
    if (response.body().find("\"field\":\"" + std::string(path) + "\"") == std::string_view::npos ||
        response.body().find("\"code\":\"" + std::string(code) + "\"") == std::string_view::npos ||
        response.body().find("\"message\":\"" + std::string(message) + "\"") == std::string_view::npos ||
        (!firstMessage.empty() &&
            (firstMessageEnd == std::string_view::npos ||
                response.body().substr(firstMessageStart, firstMessageEnd - firstMessageStart) !=
                    firstMessage))) {
        std::cerr << "expected " << path << '/' << code << '/' << message << " got " << response.body() << '\n';
        throw std::runtime_error("validation issue or first message changed");
    }
}

std::string jsonString(std::string_view value) {
    std::string result;
    for (const char ch : value) {
        switch (ch) {
            case '\\': result += "\\\\"; break;
            case '"': result += "\\\""; break;
            case '\t': result += "\\t"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\f': result += "\\f"; break;
            case '\v': result += "\\u000b"; break;
            default: result += ch; break;
        }
    }
    return result;
}

std::string payload(std::string_view name, std::string_view config =
    R"({"endpoints":[{"id":"aaaaaaaa-0000-0000-0000-000000000002","ip_address":"2001:db8::1","line_code":"default"}]})") {
    return "{\"cluster_id\":\"aaaaaaaa-0000-0000-0000-000000000001\",\"name\":\"" +
           jsonString(name) + "\",\"status\":\"enabled\",\"config\":" +
           std::string(config) + "}";
}

} // namespace

int main() try {
    ruvia::TestApp app;
    for (const std::string_view name : {"t", "n", "r", "f", "v", "节点"}) {
        const auto json = payload(name);
        const auto response = app.request(
            ruvia::TestRequest::post("/node-model-contract").json(json));
        expect(response.status() == ruvia::http_status::kOk, "valid name was rejected");
        expect(response.body() == json, "validatedJson binding did not retain the request body");
    }

    for (const std::string_view whitespace : {" ", "\t", "\n", "\r\f\v"}) {
        const auto response = app.request(ruvia::TestRequest::post("/node-model-contract")
                                              .json(payload(whitespace)));
        expect(response.status() == ruvia::http_status::kBadRequest,
               "whitespace-only node name was accepted");
        if (response.body().find(R"("field":"name","code":"regex","message":"节点名称不能为空")") ==
            std::string_view::npos) {
            std::cerr << response.body() << '\n';
            throw std::runtime_error("whitespace node-name validation issue changed");
        }
    }

    expectIssue(app,
        R"({"cluster_id":"bad","name":"edge","status":"enabled","config":{"endpoints":[{"id":"aaaaaaaa-0000-0000-0000-000000000002","ip_address":"192.0.2.1","line_code":"default"}]}})",
        "cluster_id", "regex", "所属集群不正确");
    expectIssue(app,
        R"({"cluster_id":"aaaaaaaa-0000-0000-0000-000000000001","name":"edge","status":"paused","config":{"endpoints":[{"id":"aaaaaaaa-0000-0000-0000-000000000002","ip_address":"192.0.2.1","line_code":"default"}]}})",
        "status", "regex", "节点状态不正确");
    expectIssue(app,
        R"({"name":"edge","status":"enabled","config":{"endpoints":[{"id":"aaaaaaaa-0000-0000-0000-000000000002","ip_address":"192.0.2.1","line_code":"default"}]}})",
        "cluster_id", "required", "请选择所属集群");
    expectIssue(app,
        R"({"cluster_id":null,"name":"edge","status":"enabled","config":{"endpoints":[{"id":"aaaaaaaa-0000-0000-0000-000000000002","ip_address":"192.0.2.1","line_code":"default"}]}})",
        "cluster_id", "invalid_type", "must be a string", "must be a string");
    expectIssue(app,
        R"({"cluster_id":"aaaaaaaa-0000-0000-0000-000000000001","name":null,"status":"enabled","config":{"endpoints":[{"id":"aaaaaaaa-0000-0000-0000-000000000002","ip_address":"192.0.2.1","line_code":"default"}]}})",
        "name", "invalid_type", "must be a string", "must be a string");
    expectIssue(app,
        R"({"cluster_id":"aaaaaaaa-0000-0000-0000-000000000001","name":"edge","status":null,"config":{"endpoints":[{"id":"aaaaaaaa-0000-0000-0000-000000000002","ip_address":"192.0.2.1","line_code":"default"}]}})",
        "status", "invalid_type", "must be a string", "must be a string");
    expectIssue(app,
        R"({"cluster_id":"aaaaaaaa-0000-0000-0000-000000000001","name":"edge","status":"enabled","config":null})",
        "config", "invalid_type", "must be an object", "must be an object");
    expectIssue(app,
        R"({"cluster_id":"aaaaaaaa-0000-0000-0000-000000000001","name":"edge","status":"enabled","config":{"endpoints":[{"id":"aaaaaaaa-0000-0000-0000-000000000002","ip_address":"192.0.2.1%1","line_code":"default"}]}})",
        "config.endpoints[0].ip_address", "format", "IP 地址格式不正确");
    expectIssue(app,
        R"({"cluster_id":"aaaaaaaa-0000-0000-0000-000000000001","name":"edge","status":"enabled","config":{"endpoints":[{"id":"aaaaaaaa-0000-0000-0000-000000000002","line_code":"default"}]}})",
        "config.endpoints[0].ip_address", "required", "IP 地址不能为空");
    expectIssue(app,
        R"({"cluster_id":"aaaaaaaa-0000-0000-0000-000000000001","name":"edge","status":"enabled","config":{"endpoints":[{"ip_address":"192.0.2.1","line_code":"default"}]}})",
        "config.endpoints[0].id", "format", "Endpoint ID 不正确");
    expectIssue(app,
        R"({"cluster_id":"aaaaaaaa-0000-0000-0000-000000000001","name":"edge","status":"enabled","config":{"endpoints":[{"id":null,"ip_address":"192.0.2.1","line_code":"default"}]}})",
        "config.endpoints[0].id", "invalid_type", "must be a string", "must be a string");
    expectIssue(app,
        R"({"cluster_id":"aaaaaaaa-0000-0000-0000-000000000001","name":"edge","status":"enabled","config":{"endpoints":[{"id":"bad","ip_address":"192.0.2.1","line_code":"default"}]}})",
        "config.endpoints[0].id", "format", "Endpoint ID 不正确");
    expectIssue(app,
        R"({"cluster_id":"aaaaaaaa-0000-0000-0000-000000000001","name":"edge","status":"enabled","config":{"endpoints":[{"id":"aaaaaaaa-0000-0000-0000-000000000002","ip_address":"192.0.2.1","line_code":""}]}})",
        "config.endpoints[0].line_code", "required", "请选择 DNS 线路");
    const auto overlongLine = std::string(65, 'x');
    const auto overlongLineJson = payload("edge", "{\"endpoints\":[{\"id\":\"aaaaaaaa-0000-0000-0000-000000000002\",\"ip_address\":\"192.0.2.1\",\"line_code\":\"" +
                                                   overlongLine + "\"}]}");
    expectIssue(app, overlongLineJson, "config.endpoints[0].line_code", "too_big", "DNS 线路不正确");
    expectIssue(app,
        R"({"cluster_id":"aaaaaaaa-0000-0000-0000-000000000001","name":"edge","status":"enabled","config":{"endpoints":[]}})",
        "config.endpoints", "too_small", "请至少填写一个 IP");
    expectIssue(app,
        R"({"cluster_id":"aaaaaaaa-0000-0000-0000-000000000001","name":"edge","status":"enabled","config":{}})",
        "config.endpoints", "required", "请至少填写一个 IP");
    expectIssue(app,
        R"({"cluster_id":"aaaaaaaa-0000-0000-0000-000000000001","name":"edge","status":"enabled","config":{"endpoints":null}})",
        "config.endpoints", "invalid_type", "must be an array", "must be an array");
    expectIssue(app,
        R"({"cluster_id":"aaaaaaaa-0000-0000-0000-000000000001","name":"edge","status":"enabled","config":{"endpoints":[{"id":"aaaaaaaa-0000-0000-0000-000000000002","ip_address":null,"line_code":"default"}]}})",
        "config.endpoints[0].ip_address", "invalid_type", "must be a string", "must be a string");
    expectIssue(app,
        R"({"cluster_id":"aaaaaaaa-0000-0000-0000-000000000001","name":"edge","status":"enabled","config":{"endpoints":[{"id":"aaaaaaaa-0000-0000-0000-000000000002","ip_address":"192.0.2.1","line_code":null}]}})",
        "config.endpoints[0].line_code", "invalid_type", "must be a string", "must be a string");
    expectIssue(app,
        R"({"cluster_id":"aaaaaaaa-0000-0000-0000-000000000001","name":"edge","status":"enabled","config":{"endpoints":[{"id":"aaaaaaaa-0000-0000-0000-000000000002","ip_address":"192.0.2.1","line_code":"default"},{"id":"AAAAAAAA-0000-0000-0000-000000000002","ip_address":"192.0.2.2","line_code":"default"}]}})",
        "config.endpoints", "custom", "Endpoint ID 和 IP 地址不能重复");

    std::string endpointList = "{\"endpoints\":[";
    for (int index = 1; index <= 8; ++index) {
        if (index > 1) endpointList += ',';
        endpointList += "{\"id\":\"aaaaaaaa-0000-0000-0000-00000000000" +
                        std::to_string(index) + "\",\"ip_address\":\"192.0.2." +
                        std::to_string(index) + "\",\"line_code\":\"default\"}";
    }
    endpointList += "]}";
    const auto eightEndpoints = app.request(ruvia::TestRequest::post("/node-model-contract")
                                                .json(payload("edge", endpointList)));
    expect(eightEndpoints.status() == ruvia::http_status::kOk,
           "the endpoint limit must allow eight values");
    endpointList.insert(endpointList.size() - 2,
        ",{\"id\":\"aaaaaaaa-0000-0000-0000-000000000009\",\"ip_address\":\"192.0.2.9\",\"line_code\":\"default\"}");
    expectIssue(app, payload("edge", endpointList), "config.endpoints", "too_big", "最多配置8个 IP");
    expect(!service::node_config::parseStored(R"({})"),
           "stored node configuration missing endpoints was accepted");
    expect(!service::node_config::parseStored(R"({"endpoints":null})"),
           "stored node configuration with null endpoints was accepted");
    expect(!service::node_config::parseStored(
               R"({"endpoints":[{"id":"aaaaaaaa-0000-0000-0000-000000000002","ip_address":"192.0.2.1"}]})"),
           "stored endpoint missing line_code was accepted");
    expect(!service::node_config::parseStored(
               R"({"endpoints":[{"id":"aaaaaaaa-0000-0000-0000-000000000002","ip_address":null,"line_code":"default"}]})"),
           "stored endpoint with null ip_address was accepted");
    expect(service::node_config::parseStored(
               R"({"endpoints":[{"id":"aaaaaaaa-0000-0000-0000-000000000002","ip_address":"192.0.2.1","line_code":"default"}]})").has_value(),
           "complete stored node configuration was rejected");

    const auto wrongContentType = app.request(
        ruvia::TestRequest::post("/node-model-contract").body("{}", "text/plain"));
    expect(wrongContentType.status() == ruvia::http_status::kUnsupportedMediaType,
           "JSON route content-type behavior changed");
    const auto malformed = app.request(
        ruvia::TestRequest::post("/node-model-contract").json("{"));
    expect(malformed.status() == ruvia::http_status::kBadRequest,
           "malformed JSON was accepted");

    for (const auto value : {"false", "0"}) {
        const auto json = std::string("{\"cluster_id\":\"aaaaaaaa-0000-0000-0000-000000000001\",\"name\":") +
                          value + R"(,"status":"enabled","config":{"endpoints":[{"id":"aaaaaaaa-0000-0000-0000-000000000002","ip_address":"192.0.2.1","line_code":"default"}]}})";
        const auto response = app.request(
            ruvia::TestRequest::post("/node-model-contract").json(json));
        expect(response.status() == ruvia::http_status::kBadRequest,
               "boolean/numeric name was accepted");
        expect(response.body().find(R"("field":"name","code":"invalid_type","message":"must be a string")") !=
                   std::string_view::npos,
               "boolean/numeric name did not retain invalid_type semantics");
    }
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
