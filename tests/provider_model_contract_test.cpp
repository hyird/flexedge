#include <cstdio>
#include <regex>
#include <string>
#include <memory_resource>
#include <string_view>
#include <vector>

#include <ruvia/web/App.h>
#include <ruvia/web/Controller.h>
#include <ruvia/web/Testing.h>

#include "service/domains/auth/auth.schema.h"
#include "service/domains/provider/certificate_provider.schema.h"
#include "service/domains/provider/dns_provider.schema.h"

namespace {

class ProviderContractController final : public ruvia::Controller<ProviderContractController> {
  public:
    RUVIA_CONTROLLER_GROUP("/provider-contract")
    RUVIA_ROUTES_BEGIN
    RUVIA_POST("/certificate-create", certificateCreate,
               ruvia::JsonBody<service::provider::CreateCertificateProviderBody>,
               service::provider::CreateCertificateProviderValidator);
    RUVIA_PUT("/certificate-update", certificateUpdate,
              ruvia::JsonBody<service::provider::UpdateCertificateProviderBody>,
              service::provider::UpdateCertificateProviderValidator);
    RUVIA_POST("/dns-create", dnsCreate,
               ruvia::JsonBody<service::provider::CreateDnsProviderBody>,
               service::provider::CreateDnsProviderValidator);
    RUVIA_PUT("/dns-update", dnsUpdate, ruvia::JsonBody<service::provider::UpdateDnsProviderBody>,
              service::provider::UpdateDnsProviderValidator);
    RUVIA_POST("/login", login, ruvia::JsonBody<service::auth::LoginBody>,
               service::auth::LoginValidator);
    RUVIA_ROUTES_END

  private:
    ruvia::Task<ruvia::HttpResponse> certificateCreate(ruvia::Context& context) {
        const auto body = context.req().validatedJson<service::provider::CreateCertificateProviderBody>();
        co_return context.text(std::string_view(body.value().get<"provider">() ? "bound" : "missing"));
    }
    ruvia::Task<ruvia::HttpResponse> certificateUpdate(ruvia::Context& context) {
        const auto body = context.req().validatedJson<service::provider::UpdateCertificateProviderBody>();
        co_return context.text(std::string_view(body.value().get<"credentialMode">() ? "bound" : "missing"));
    }
    ruvia::Task<ruvia::HttpResponse> dnsCreate(ruvia::Context& context) {
        const auto body = context.req().validatedJson<service::provider::CreateDnsProviderBody>();
        co_return context.text(std::string_view(body.value().get<"name">() ? "bound" : "missing"));
    }
    ruvia::Task<ruvia::HttpResponse> dnsUpdate(ruvia::Context& context) {
        const auto body = context.req().validatedJson<service::provider::UpdateDnsProviderBody>();
        co_return context.text(std::string_view(body.value().get<"name">() ? "bound" : "missing"));
    }
    ruvia::Task<ruvia::HttpResponse> login(ruvia::Context& context) {
        const auto body = context.req().validatedJson<service::auth::LoginBody>();
        co_return context.text(std::string_view(body.value().get<"username">() ? "bound" : "missing"));
    }
};

int failures{};
void expect(bool condition, std::string_view description) {
    if (condition) return;
    ++failures;
    std::fprintf(stderr, "FAILED: %.*s\n", static_cast<int>(description.size()), description.data());
}

std::string issues(const ruvia::HttpErrorInfo& error) {
    std::string result;
    for (const auto& issue : error.validationIssues()) {
        result.append(issue.field()).push_back('|');
        result.append(issue.code()).push_back('|');
        result.append(issue.message()).push_back('\n');
    }
    return result;
}

void captureErrors(ruvia::TestApp& app) {
    app.onError([](ruvia::Context& context, ruvia::HttpErrorInfo error)
                    -> ruvia::Task<ruvia::HttpResponse> {
        context.status(error.status());
        std::pmr::string body(issues(error), context.arena());
        co_return context.text(std::move(body));
    });
}

void expectFirstIssue(const ruvia::TestResponse& response, std::string_view field,
                      std::string_view code, std::string_view message) {
    const std::string expected = std::string(field) + "|" + std::string(code) + "|" +
                                 std::string(message) + "\n";
    if (response.status() != ruvia::http_status::kBadRequest ||
        !response.body().starts_with(expected)) {
        ++failures;
        std::fprintf(stderr, "FAILED first issue: %s; got status=%u body=%.*s\n",
                     expected.c_str(), response.status().value(),
                     static_cast<int>(response.body().size()), response.body().data());
    }
}

void expectIssue(const ruvia::TestResponse& response, std::string_view field,
                 std::string_view code, std::string_view message) {
    const std::string expected = std::string(field) + "|" + std::string(code) + "|" +
                                 std::string(message) + "\n";
    if (response.status() != ruvia::http_status::kBadRequest ||
        response.body().find(expected) == std::string_view::npos) {
        ++failures;
        std::fprintf(stderr, "FAILED: %s; got status=%u body=%.*s\n", expected.c_str(),
                     response.status().value(), static_cast<int>(response.body().size()),
                     response.body().data());
    }
}

} // namespace

int main() {
    ruvia::TestApp app;
    captureErrors(app);
    constexpr auto certificateCreate = "/provider-contract/certificate-create";
    constexpr auto certificateUpdate = "/provider-contract/certificate-update";
    constexpr auto dnsCreate = "/provider-contract/dns-create";
    constexpr auto dnsUpdate = "/provider-contract/dns-update";
    constexpr auto login = "/provider-contract/login";

    expectIssue(app.request(ruvia::TestRequest::post(certificateCreate).json("{}")),
                "provider", "required", "请选择供应商类型");
    expectFirstIssue(app.request(ruvia::TestRequest::post(certificateCreate).json(R"({"provider":null})")),
                     "provider", "invalid_type", "must be a string");
    expectIssue(app.request(ruvia::TestRequest::post(certificateCreate).json(
                    R"({"provider":"bad","credential_mode":"email"})")),
                "provider", "regex", "供应商类型不正确");
    expectIssue(app.request(ruvia::TestRequest::put(certificateUpdate).json("{}")),
                "credential_mode", "required", "请选择接入方式");
    expectIssue(app.request(ruvia::TestRequest::put(certificateUpdate).json(
                    R"({"credential_mode":"email","account_email":"x@example.com."})")),
                "account_email", "regex", "账户邮箱格式不正确");
    const auto omittedCredentials = app.request(ruvia::TestRequest::post(certificateCreate).json(
        R"({"provider":"letsencrypt","credential_mode":"email"})"));
    expect(omittedCredentials.status() == ruvia::http_status::kOk &&
               omittedCredentials.body() == "bound",
           "omitted optional certificate credentials remain accepted");
    expectFirstIssue(app.request(ruvia::TestRequest::put(certificateUpdate).json(
                         R"({"credential_mode":"access_key","access_key":null})")),
                     "access_key", "invalid_type", "must be a string");
    expectFirstIssue(app.request(ruvia::TestRequest::post(certificateCreate).json(
                         R"({"provider":"letsencrypt","credential_mode":"email","account_email":null})")),
                     "account_email", "invalid_type", "must be a string");

    expectIssue(app.request(ruvia::TestRequest::post(dnsCreate).json("{}")),
                "name", "required", "账号名称不能为空");
    expectIssue(app.request(ruvia::TestRequest::post(dnsCreate).json(R"({"name":"edge"})")),
                "provider", "required", "服务商类型不能为空");
    expectIssue(app.request(ruvia::TestRequest::post(dnsCreate).json(
                    R"({"name":"edge","provider":"cloudflare"})")),
                "account_id", "required", "账户标识不能为空");
    expectIssue(app.request(ruvia::TestRequest::post(dnsCreate).json(
                    R"({"name":"edge","provider":"cloudflare","account_id":"account-1"})")),
                "api_token", "required", "访问密钥不能为空");
    expectIssue(app.request(ruvia::TestRequest::put(dnsUpdate).json("{}")),
                "name", "required", "账号名称不能为空");
    expectFirstIssue(app.request(ruvia::TestRequest::put(dnsUpdate).json(
                         R"({"name":"edge","api_token":null})")),
                     "api_token", "invalid_type", "must be a string");
    expectFirstIssue(app.request(ruvia::TestRequest::post(dnsCreate).json(
                         R"({"name":null,"provider":"cloudflare","account_id":"account-1","api_token":"1234567890123456"})")),
                     "name", "invalid_type", "must be a string");

    expectIssue(app.request(ruvia::TestRequest::post(login).json("{}")),
                "username", "required", "用户名不能为空");
    expectFirstIssue(app.request(ruvia::TestRequest::post(login).json(R"({"username":null})")),
                     "username", "invalid_type", "must be a string");
    expectIssue(app.request(ruvia::TestRequest::post(login).json(R"({"username":"admin"})")),
                "password", "required", "密码不能为空");

    const std::regex oldEmail(
        R"(^[A-Za-z0-9.!#$%&'*+/=?^_`{|}~-]+@[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?(?:\.[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?)+$)");
    const std::vector<std::string> emailCases{
        "a@example.com", "user+tag@sub.example-domain.com", "a..b@example.com",
        "a@b.c", "a@b.c.", "a@b..c", "a@-b.c", "a@b-.c", "a@localhost",
        "@example.com", "a@@example.com", "a b@example.com", "a@example-.com"};
    for (const auto& value : emailCases) {
        const bool oldBehavior = std::regex_match(value, oldEmail);
        expect(service::provider::isProviderEmail(value) == oldBehavior,
               std::string("email predicate differs from legacy regex: ") + value);
    }
    expect(!service::provider::isProviderEmail("a@b.c."),
           "email domain trailing dot rejected as by legacy regex");
    return failures == 0 ? 0 : 1;
}
