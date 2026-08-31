#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>

#include <ruvia/http/HttpKnownMethod.h>
#include <ruvia/web/Model.h>

#include "service/config/outbound.h"
#include "service/features/background/worker_pool.h"
#include "service/features/outbound_http/client.h"

namespace service::website_dns {

RUVIA_REQUEST_MODEL(DnsDohAnswerInput, RUVIA_OPTIONAL_FIELD(type, ruvia::Int64),
                    RUVIA_OPTIONAL_FIELD(data, ruvia::String));
RUVIA_REQUEST_MODEL(DnsDohEnvelopeInput, RUVIA_OPTIONAL_FIELD_NAME("Status", status, ruvia::Int64),
                    RUVIA_OPTIONAL_FIELD_NAME("Answer", answer, ruvia::Array<DnsDohAnswerInput>));

struct ProbeResult final {
    bool matched;
    std::string queryName;
    std::string observedTarget;
};

inline std::string normalizeDnsName(std::string_view value) {
    while (!value.empty() && value.back() == '.') {
        value.remove_suffix(1);
    }
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return result;
}

inline std::string probeName(std::string_view hostname, std::string_view websiteDomainId) {
    if (!hostname.starts_with("*.")) {
        return std::string(hostname);
    }
    std::string token;
    token.reserve(8);
    for (const auto ch : websiteDomainId) {
        if (ch == '-') {
            continue;
        }
        token.push_back(ch);
        if (token.size() == 8) {
            break;
        }
    }
    return "flexedge-" + token + "." + std::string(hostname.substr(2));
}

inline ruvia::Task<ProbeResult> probeCname(service::background::WorkerContext& context,
                                           std::string_view hostname,
                                           std::string_view expectedTarget,
                                           std::string_view websiteDomainId) {
    const auto queryName = probeName(hostname, websiteDomainId);
    auto&& client = context.httpClient(service::config::kAliDnsDohOriginAlias);
    std::array<ruvia::HttpHeaderView, 1> headers{
        ruvia::HttpHeaderView{"accept", "application/dns-json"}};
    const auto target = "/resolve?name=" + queryName + "&type=CNAME&cd=0&do=0";
    const auto response = co_await service::outbound_http::sendBuffered(
        client,
        {
            .method = ruvia::knownHttpMethodToken(ruvia::HttpKnownMethod::kGet),
            .target = target,
            .headers = headers,
            .content = ruvia::HttpClientRequestContentView::none(),
        },
        {.timeout = std::chrono::seconds(10), .stopToken = context.stopToken()});
    if (!response.status().isSuccessful()) {
        throw std::runtime_error("公共 DNS 探测服务返回异常");
    }
    const std::optional<DnsDohEnvelopeInput> parsed =
        ruvia::fromJson<DnsDohEnvelopeInput>(response.body(), {.resource = context.resource()});
    if (!parsed || !parsed->get<"status">()) {
        throw std::runtime_error("公共 DNS 探测响应无效");
    }

    const auto expected = normalizeDnsName(expectedTarget);
    std::string observed;
    if (const auto& answers = parsed->get<"answer">(); answers) {
        for (const auto& answer : *answers) {
            const auto& type = answer.get<"type">();
            const auto& data = answer.get<"data">();
            if (!type || static_cast<std::int64_t>(*type) != 5 || !data) {
                continue;
            }
            const auto answerTarget = normalizeDnsName(data->view());
            if (observed.empty()) {
                observed = answerTarget;
            }
            if (answerTarget == expected) {
                co_return ProbeResult{true, queryName, answerTarget};
            }
        }
    }
    co_return ProbeResult{false, queryName, observed};
}

} // namespace service::website_dns
