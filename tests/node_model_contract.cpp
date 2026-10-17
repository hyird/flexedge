#include <ruvia/web/controller.h>
#include <ruvia/web/testing.h>

#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#include "service/domains/node/node_types.h"
#include "service/features/node_runtime/mapper.h"
#include "service/features/node_runtime/transport.h"

namespace {

class node_model_contract_controller final {
public:
    RUVIA_CONTROLLER_GROUP("/node-model-contract")
    RUVIA_ROUTES_BEGIN(node_model_contract_controller)
    RUVIA_POST("/", validate, ruvia::json_body<service::node::node_save_input>);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> validate(ruvia::context& context) {
        const auto body = context.req().validated_json<service::node::node_save_input>();
        ruvia::validator validator({.resource_ = context.arena()});
        service::node::validate_node_save_input(body.value(), validator);
        validator.throw_if_invalid();
        co_return context.text(body.raw());
    }
};

void expect(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

void expect_issue(ruvia::test_app& app, std::string_view json, std::string_view path,
    std::string_view code, std::string_view message,
    std::string_view first_message = {}) {
    const auto response = app.request(
        ruvia::test_request::post("/node-model-contract").json(json));
    expect(response.status() == ruvia::http_status::bad_request, "invalid node input was accepted");
    const auto first_message_key = response.body().find(R"("message":")");
    const auto first_message_start = first_message_key == std::string_view::npos
                                         ? first_message_key
                                         : first_message_key + std::string_view(R"("message":")").size();
    const auto first_message_end = first_message_start == std::string_view::npos
                                       ? first_message_start
                                       : response.body().find('"', first_message_start);
    if (response.body().find("\"field\":\"" + std::string(path) + "\"") == std::string_view::npos ||
        response.body().find("\"code\":\"" + std::string(code) + "\"") == std::string_view::npos ||
        response.body().find("\"message\":\"" + std::string(message) + "\"") == std::string_view::npos ||
        (!first_message.empty() &&
            (first_message_end == std::string_view::npos ||
                response.body().substr(first_message_start, first_message_end - first_message_start) !=
                    first_message))) {
        std::cerr << "expected " << path << '/' << code << '/' << message << " got " << response.body() << '\n';
        throw std::runtime_error("validation issue or first message changed");
    }
}

std::string json_string(std::string_view value) {
    std::string result;
    for (const char ch : value) {
        switch (ch) {
            case '\\':
                result += "\\\\";
                break;
            case '"':
                result += "\\\"";
                break;
            case '\t':
                result += "\\t";
                break;
            case '\n':
                result += "\\n";
                break;
            case '\r':
                result += "\\r";
                break;
            case '\f':
                result += "\\f";
                break;
            case '\v':
                result += "\\u000b";
                break;
            default:
                result += ch;
                break;
        }
    }
    return result;
}

std::string payload(std::string_view name, std::string_view config =
                                               R"({"endpoints":[{"id":"aaaaaaaa-0000-0000-0000-000000000002","ip_address":"2001:db8::1","line_code":"default"}]})") {
    return "{\"cluster_id\":\"aaaaaaaa-0000-0000-0000-000000000001\",\"name\":\"" +
           json_string(name) + "\",\"status\":\"enabled\",\"config\":" +
           std::string(config) + "}";
}

}  // namespace

int main() try {
    ruvia::test_app app;
    for (const std::string_view name : {"t", "n", "r", "f", "v", "节点"}) {
        const auto json = payload(name);
        const auto response = app.request(
            ruvia::test_request::post("/node-model-contract").json(json));
        expect(response.status() == ruvia::http_status::ok, "valid name was rejected");
        expect(response.body() == json, "validatedJson binding did not retain the request body");
    }

    for (const std::string_view whitespace : {" ", "\t", "\n", "\r\f\v"}) {
        const auto response = app.request(ruvia::test_request::post("/node-model-contract")
                .json(payload(whitespace)));
        expect(response.status() == ruvia::http_status::bad_request,
            "whitespace-only node name was accepted");
        if (response.body().find(R"("field":"name","code":"regex","message":"节点名称不能为空")") ==
            std::string_view::npos) {
            std::cerr << response.body() << '\n';
            throw std::runtime_error("whitespace node-name validation issue changed");
        }
    }

    expect_issue(app,
        R"({"cluster_id":"bad","name":"edge","status":"enabled","config":{"endpoints":[{"id":"aaaaaaaa-0000-0000-0000-000000000002","ip_address":"192.0.2.1","line_code":"default"}]}})",
        "cluster_id", "regex", "所属集群不正确");
    expect_issue(app,
        R"({"cluster_id":"aaaaaaaa-0000-0000-0000-000000000001","name":"edge","status":"paused","config":{"endpoints":[{"id":"aaaaaaaa-0000-0000-0000-000000000002","ip_address":"192.0.2.1","line_code":"default"}]}})",
        "status", "regex", "节点状态不正确");
    expect_issue(app,
        R"({"name":"edge","status":"enabled","config":{"endpoints":[{"id":"aaaaaaaa-0000-0000-0000-000000000002","ip_address":"192.0.2.1","line_code":"default"}]}})",
        "cluster_id", "required", "请选择所属集群");
    expect_issue(app,
        R"({"cluster_id":null,"name":"edge","status":"enabled","config":{"endpoints":[{"id":"aaaaaaaa-0000-0000-0000-000000000002","ip_address":"192.0.2.1","line_code":"default"}]}})",
        "cluster_id", "invalid_type", "must be a string", "must be a string");
    expect_issue(app,
        R"({"cluster_id":"aaaaaaaa-0000-0000-0000-000000000001","name":null,"status":"enabled","config":{"endpoints":[{"id":"aaaaaaaa-0000-0000-0000-000000000002","ip_address":"192.0.2.1","line_code":"default"}]}})",
        "name", "invalid_type", "must be a string", "must be a string");
    expect_issue(app,
        R"({"cluster_id":"aaaaaaaa-0000-0000-0000-000000000001","name":"edge","status":null,"config":{"endpoints":[{"id":"aaaaaaaa-0000-0000-0000-000000000002","ip_address":"192.0.2.1","line_code":"default"}]}})",
        "status", "invalid_type", "must be a string", "must be a string");
    expect_issue(app,
        R"({"cluster_id":"aaaaaaaa-0000-0000-0000-000000000001","name":"edge","status":"enabled","config":null})",
        "config", "invalid_type", "must be an object", "must be an object");
    expect_issue(app,
        R"({"cluster_id":"aaaaaaaa-0000-0000-0000-000000000001","name":"edge","status":"enabled","config":{"endpoints":[{"id":"aaaaaaaa-0000-0000-0000-000000000002","ip_address":"192.0.2.1%1","line_code":"default"}]}})",
        "config.endpoints[0].ip_address", "format", "IP 地址格式不正确");
    expect_issue(app,
        R"({"cluster_id":"aaaaaaaa-0000-0000-0000-000000000001","name":"edge","status":"enabled","config":{"endpoints":[{"id":"aaaaaaaa-0000-0000-0000-000000000002","line_code":"default"}]}})",
        "config.endpoints[0].ip_address", "required", "IP 地址不能为空");
    expect_issue(app,
        R"({"cluster_id":"aaaaaaaa-0000-0000-0000-000000000001","name":"edge","status":"enabled","config":{"endpoints":[{"ip_address":"192.0.2.1","line_code":"default"}]}})",
        "config.endpoints[0].id", "format", "Endpoint ID 不正确");
    expect_issue(app,
        R"({"cluster_id":"aaaaaaaa-0000-0000-0000-000000000001","name":"edge","status":"enabled","config":{"endpoints":[{"id":null,"ip_address":"192.0.2.1","line_code":"default"}]}})",
        "config.endpoints[0].id", "invalid_type", "must be a string", "must be a string");
    expect_issue(app,
        R"({"cluster_id":"aaaaaaaa-0000-0000-0000-000000000001","name":"edge","status":"enabled","config":{"endpoints":[{"id":"bad","ip_address":"192.0.2.1","line_code":"default"}]}})",
        "config.endpoints[0].id", "format", "Endpoint ID 不正确");
    expect_issue(app,
        R"({"cluster_id":"aaaaaaaa-0000-0000-0000-000000000001","name":"edge","status":"enabled","config":{"endpoints":[{"id":"aaaaaaaa-0000-0000-0000-000000000002","ip_address":"192.0.2.1","line_code":""}]}})",
        "config.endpoints[0].line_code", "required", "请选择 DNS 线路");
    const auto overlong_line = std::string(65, 'x');
    const auto overlong_line_json = payload("edge", "{\"endpoints\":[{\"id\":\"aaaaaaaa-0000-0000-0000-000000000002\",\"ip_address\":\"192.0.2.1\",\"line_code\":\"" +
                                                        overlong_line + "\"}]}");
    expect_issue(app, overlong_line_json, "config.endpoints[0].line_code", "too_big", "DNS 线路不正确");
    expect_issue(app,
        R"({"cluster_id":"aaaaaaaa-0000-0000-0000-000000000001","name":"edge","status":"enabled","config":{"endpoints":[]}})",
        "config.endpoints", "too_small", "请至少填写一个 IP");
    expect_issue(app,
        R"({"cluster_id":"aaaaaaaa-0000-0000-0000-000000000001","name":"edge","status":"enabled","config":{}})",
        "config.endpoints", "required", "请至少填写一个 IP");
    expect_issue(app,
        R"({"cluster_id":"aaaaaaaa-0000-0000-0000-000000000001","name":"edge","status":"enabled","config":{"endpoints":null}})",
        "config.endpoints", "invalid_type", "must be an array", "must be an array");
    expect_issue(app,
        R"({"cluster_id":"aaaaaaaa-0000-0000-0000-000000000001","name":"edge","status":"enabled","config":{"endpoints":[{"id":"aaaaaaaa-0000-0000-0000-000000000002","ip_address":null,"line_code":"default"}]}})",
        "config.endpoints[0].ip_address", "invalid_type", "must be a string", "must be a string");
    expect_issue(app,
        R"({"cluster_id":"aaaaaaaa-0000-0000-0000-000000000001","name":"edge","status":"enabled","config":{"endpoints":[{"id":"aaaaaaaa-0000-0000-0000-000000000002","ip_address":"192.0.2.1","line_code":null}]}})",
        "config.endpoints[0].line_code", "invalid_type", "must be a string", "must be a string");
    expect_issue(app,
        R"({"cluster_id":"aaaaaaaa-0000-0000-0000-000000000001","name":"edge","status":"enabled","config":{"endpoints":[{"id":"aaaaaaaa-0000-0000-0000-000000000002","ip_address":"192.0.2.1","line_code":"default"},{"id":"AAAAAAAA-0000-0000-0000-000000000002","ip_address":"192.0.2.2","line_code":"default"}]}})",
        "config.endpoints", "custom", "Endpoint ID 和 IP 地址不能重复");

    std::string endpoint_list = "{\"endpoints\":[";
    for (int index = 1; index <= 8; ++index) {
        if (index > 1) {
            endpoint_list += ',';
        }
        endpoint_list += "{\"id\":\"aaaaaaaa-0000-0000-0000-00000000000" +
                         std::to_string(index) + "\",\"ip_address\":\"192.0.2." +
                         std::to_string(index) + "\",\"line_code\":\"default\"}";
    }
    endpoint_list += "]}";
    const auto eight_endpoints = app.request(ruvia::test_request::post("/node-model-contract")
            .json(payload("edge", endpoint_list)));
    expect(eight_endpoints.status() == ruvia::http_status::ok,
        "the endpoint limit must allow eight values");
    endpoint_list.insert(endpoint_list.size() - 2,
        ",{\"id\":\"aaaaaaaa-0000-0000-0000-000000000009\",\"ip_address\":\"192.0.2.9\",\"line_code\":\"default\"}");
    expect_issue(app, payload("edge", endpoint_list), "config.endpoints", "too_big", "最多配置8个 IP");
    expect(!service::node_config::parse_stored(R"({})"),
        "stored node configuration missing endpoints was accepted");
    expect(!service::node_config::parse_stored(R"({"endpoints":null})"),
        "stored node configuration with null endpoints was accepted");
    expect(!service::node_config::parse_stored(
               R"({"endpoints":[{"id":"aaaaaaaa-0000-0000-0000-000000000002","ip_address":"192.0.2.1"}]})"),
        "stored endpoint missing line_code was accepted");
    expect(!service::node_config::parse_stored(
               R"({"endpoints":[{"id":"aaaaaaaa-0000-0000-0000-000000000002","ip_address":null,"line_code":"default"}]})"),
        "stored endpoint with null ip_address was accepted");
    expect(service::node_config::parse_stored(
               R"({"endpoints":[{"id":"aaaaaaaa-0000-0000-0000-000000000002","ip_address":"192.0.2.1","line_code":"default"}]})")
               .has_value(),
        "complete stored node configuration was rejected");

    // connected_ip_count round-trips through stored runtime JSON: 0 is kept, absence (nodes
    // before 0.4.1) stays absent, and the node runtime DTO emits 0 rather than omitting it.
    service::node_runtime::node_runtime_output stored_runtime;
    stored_runtime.set<"connection_count">(2);
    stored_runtime.set<"connected_ip_count">(0);
    stored_runtime.set<"cpu_count">(4);
    stored_runtime.set<"memory_total_bytes">(8LL << 30);
    stored_runtime.set<"disk_total_bytes">(100LL << 30);
    const auto stored_json = std::string(ruvia::to_json(stored_runtime));
    expect(stored_json.find("\"connected_ip_count\":0") != std::string::npos,
        "stored runtime must keep a zero connected IP count");
    const auto parsed_runtime = service::node_runtime::parse_stored(stored_json);
    expect(parsed_runtime && parsed_runtime->cpu_count_ == 4 &&
               parsed_runtime->memory_total_bytes_ == (8LL << 30) &&
               parsed_runtime->disk_total_bytes_ == (100LL << 30),
        "host capacity did not round-trip through stored runtime");
    expect(parsed_runtime && parsed_runtime->connected_ip_count_ == 0 &&
               parsed_runtime->connection_count_ == 2,
        "stored connected IP count did not round-trip");
    const auto legacy_runtime = service::node_runtime::parse_stored(R"({"connection_count":2})");
    expect(legacy_runtime && !legacy_runtime->cpu_count_ &&
               !legacy_runtime->memory_total_bytes_ && !legacy_runtime->disk_total_bytes_,
        "legacy runtime must not invent host capacity");
    expect(legacy_runtime && !legacy_runtime->connected_ip_count_,
        "a runtime without connected IP count must stay without it");
    service::node_runtime::node_runtime_dto runtime_dto;
    runtime_dto.set<"registration_status">("registered");
    runtime_dto.set<"connection_status">("online");
    runtime_dto.set<"applied_node_spec_revision">(1);
    runtime_dto.set<"connected_ip_count">(0);
    expect(std::string(ruvia::to_json(runtime_dto)).find("\"connected_ip_count\":0") != std::string::npos,
        "node runtime DTO must emit a zero connected IP count");

    const auto wrong_content_type = app.request(
        ruvia::test_request::post("/node-model-contract").body("{}", "text/plain"));
    expect(wrong_content_type.status() == ruvia::http_status::unsupported_media_type,
        "JSON route content-type behavior changed");
    const auto malformed = app.request(
        ruvia::test_request::post("/node-model-contract").json("{"));
    expect(malformed.status() == ruvia::http_status::bad_request,
        "malformed JSON was accepted");

    for (const auto value : {"false", "0"}) {
        const auto json = std::string("{\"cluster_id\":\"aaaaaaaa-0000-0000-0000-000000000001\",\"name\":") +
                          value + R"(,"status":"enabled","config":{"endpoints":[{"id":"aaaaaaaa-0000-0000-0000-000000000002","ip_address":"192.0.2.1","line_code":"default"}]}})";
        const auto response = app.request(
            ruvia::test_request::post("/node-model-contract").json(json));
        expect(response.status() == ruvia::http_status::bad_request,
            "boolean/numeric name was accepted");
        expect(response.body().find(R"("field":"name","code":"invalid_type","message":"must be a string")") !=
                   std::string_view::npos,
            "boolean/numeric name did not retain invalid_type semantics");
    }
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
