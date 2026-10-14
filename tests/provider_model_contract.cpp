#include <ruvia/web/app.h>
#include <ruvia/web/controller.h>
#include <ruvia/web/testing.h>

#include <cstdio>
#include <memory_resource>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

#include "service/domains/auth/auth_schema.h"
#include "service/domains/provider/certificate_provider_schema.h"
#include "service/domains/provider/dns_provider_schema.h"

namespace {

class provider_contract_controller final {
public:
    RUVIA_CONTROLLER_GROUP("/provider-contract")
    RUVIA_ROUTES_BEGIN(provider_contract_controller)
    RUVIA_POST("/certificate-create", certificate_create,
        ruvia::json_body<service::provider::create_certificate_provider_body>,
        service::provider::create_certificate_provider_validator);
    RUVIA_PUT("/certificate-update", certificate_update,
        ruvia::json_body<service::provider::update_certificate_provider_body>,
        service::provider::update_certificate_provider_validator);
    RUVIA_POST("/dns-create", dns_create,
        ruvia::json_body<service::provider::create_dns_provider_body>,
        service::provider::create_dns_provider_validator);
    RUVIA_PUT("/dns-update", dns_update, ruvia::json_body<service::provider::update_dns_provider_body>,
        service::provider::update_dns_provider_validator);
    RUVIA_POST("/login", login, ruvia::json_body<service::auth::login_body>,
        service::auth::login_validator);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> certificate_create(ruvia::context& context) {
        const auto body = context.req().validated_json<service::provider::create_certificate_provider_body>();
        co_return context.text(std::string_view(body.value().get<"provider">() ? "bound" : "missing"));
    }
    ruvia::task<ruvia::http_response> certificate_update(ruvia::context& context) {
        const auto body = context.req().validated_json<service::provider::update_certificate_provider_body>();
        co_return context.text(std::string_view(body.value().get<"credential_mode">() ? "bound" : "missing"));
    }
    ruvia::task<ruvia::http_response> dns_create(ruvia::context& context) {
        const auto body = context.req().validated_json<service::provider::create_dns_provider_body>();
        co_return context.text(std::string_view(body.value().get<"name">() ? "bound" : "missing"));
    }
    ruvia::task<ruvia::http_response> dns_update(ruvia::context& context) {
        const auto body = context.req().validated_json<service::provider::update_dns_provider_body>();
        co_return context.text(std::string_view(body.value().get<"name">() ? "bound" : "missing"));
    }
    ruvia::task<ruvia::http_response> login(ruvia::context& context) {
        const auto body = context.req().validated_json<service::auth::login_body>();
        co_return context.text(std::string_view(body.value().get<"username">() ? "bound" : "missing"));
    }
};

int failures{};
void expect(bool condition, std::string_view description) {
    if (condition) {
        return;
    }
    ++failures;
    std::fprintf(stderr, "FAILED: %.*s\n", static_cast<int>(description.size()), description.data());
}

std::string issues(const ruvia::http_error_info& error) {
    std::string result;
    for (const auto& issue : error.validation_issues()) {
        result.append(issue.field()).push_back('|');
        result.append(issue.code()).push_back('|');
        result.append(issue.message()).push_back('\n');
    }
    return result;
}

void capture_errors(ruvia::test_app& app) {
    app.on_error([](ruvia::context& context, ruvia::http_error_info error)
                     -> ruvia::task<ruvia::http_response> {
        context.status(error.status());
        std::pmr::string body(issues(error), context.arena());
        co_return context.text(std::move(body));
    });
}

void expect_first_issue(const ruvia::test_response& response, std::string_view field,
    std::string_view code, std::string_view message) {
    const std::string expected = std::string(field) + "|" + std::string(code) + "|" +
                                 std::string(message) + "\n";
    if (response.status() != ruvia::http_status::bad_request ||
        !response.body().starts_with(expected)) {
        ++failures;
        std::fprintf(stderr, "FAILED first issue: %s; got status=%u body=%.*s\n",
            expected.c_str(), response.status().value(),
            static_cast<int>(response.body().size()), response.body().data());
    }
}

void expect_issue(const ruvia::test_response& response, std::string_view field,
    std::string_view code, std::string_view message) {
    const std::string expected = std::string(field) + "|" + std::string(code) + "|" +
                                 std::string(message) + "\n";
    if (response.status() != ruvia::http_status::bad_request ||
        response.body().find(expected) == std::string_view::npos) {
        ++failures;
        std::fprintf(stderr, "FAILED: %s; got status=%u body=%.*s\n", expected.c_str(),
            response.status().value(), static_cast<int>(response.body().size()),
            response.body().data());
    }
}

}  // namespace

int main() {
    ruvia::test_app app;
    capture_errors(app);
    constexpr auto certificate_create = "/provider-contract/certificate-create";
    constexpr auto certificate_update = "/provider-contract/certificate-update";
    constexpr auto dns_create = "/provider-contract/dns-create";
    constexpr auto dns_update = "/provider-contract/dns-update";
    constexpr auto login = "/provider-contract/login";

    expect_issue(app.request(ruvia::test_request::post(certificate_create).json("{}")),
        "provider", "required", "请选择供应商类型");
    expect_first_issue(app.request(ruvia::test_request::post(certificate_create).json(R"({"provider":null})")),
        "provider", "invalid_type", "must be a string");
    expect_issue(app.request(ruvia::test_request::post(certificate_create).json(R"({"provider":"bad","credential_mode":"email"})")),
        "provider", "regex", "供应商类型不正确");
    expect_issue(app.request(ruvia::test_request::put(certificate_update).json("{}")),
        "credential_mode", "required", "请选择接入方式");
    expect_issue(app.request(ruvia::test_request::put(certificate_update).json(R"({"credential_mode":"email","account_email":"x@example.com."})")),
        "account_email", "regex", "账户邮箱格式不正确");
    const auto omitted_credentials = app.request(ruvia::test_request::post(certificate_create).json(R"({"provider":"letsencrypt","credential_mode":"email"})"));
    expect(omitted_credentials.status() == ruvia::http_status::ok &&
               omitted_credentials.body() == "bound",
        "omitted optional certificate credentials remain accepted");
    expect_first_issue(app.request(ruvia::test_request::put(certificate_update).json(R"({"credential_mode":"access_key","access_key":null})")),
        "access_key", "invalid_type", "must be a string");
    expect_first_issue(app.request(ruvia::test_request::post(certificate_create).json(R"({"provider":"letsencrypt","credential_mode":"email","account_email":null})")),
        "account_email", "invalid_type", "must be a string");

    expect_issue(app.request(ruvia::test_request::post(dns_create).json("{}")),
        "name", "required", "账号名称不能为空");
    expect_issue(app.request(ruvia::test_request::post(dns_create).json(R"({"name":"edge"})")),
        "provider", "required", "服务商类型不能为空");
    expect_issue(app.request(ruvia::test_request::post(dns_create).json(R"({"name":"edge","provider":"cloudflare"})")),
        "account_id", "required", "账户标识不能为空");
    expect_issue(app.request(ruvia::test_request::post(dns_create).json(R"({"name":"edge","provider":"cloudflare","account_id":"account-1"})")),
        "api_token", "required", "访问密钥不能为空");
    expect_issue(app.request(ruvia::test_request::put(dns_update).json("{}")),
        "name", "required", "账号名称不能为空");
    expect_first_issue(app.request(ruvia::test_request::put(dns_update).json(R"({"name":"edge","api_token":null})")),
        "api_token", "invalid_type", "must be a string");
    expect_first_issue(app.request(ruvia::test_request::post(dns_create).json(R"({"name":null,"provider":"cloudflare","account_id":"account-1","api_token":"1234567890123456"})")),
        "name", "invalid_type", "must be a string");

    expect_issue(app.request(ruvia::test_request::post(login).json("{}")),
        "username", "required", "用户名不能为空");
    expect_first_issue(app.request(ruvia::test_request::post(login).json(R"({"username":null})")),
        "username", "invalid_type", "must be a string");
    expect_issue(app.request(ruvia::test_request::post(login).json(R"({"username":"admin"})")),
        "password", "required", "密码不能为空");

    const std::regex old_email(
        R"(^[A-Za-z0-9.!#$%&'*+/=?^_`{|}~-]+@[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?(?:\.[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?)+$)");
    const std::vector<std::string> email_cases{
        "a@example.com", "user+tag@sub.example-domain.com", "a..b@example.com",
        "a@b.c", "a@b.c.", "a@b..c", "a@-b.c", "a@b-.c", "a@localhost",
        "@example.com", "a@@example.com", "a b@example.com", "a@example-.com"};
    for (const auto& value : email_cases) {
        const bool old_behavior = std::regex_match(value, old_email);
        expect(service::provider::is_provider_email(value) == old_behavior,
            std::string("email predicate differs from legacy regex: ") + value);
    }
    expect(!service::provider::is_provider_email("a@b.c."),
        "email domain trailing dot rejected as by legacy regex");
    return failures == 0 ? 0 : 1;
}
