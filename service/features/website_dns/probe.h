#pragma once

#include <ruvia/http/http_known_method.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>

#include "ruvia/web/model.h"

#include "service/config/outbound.h"
#include "service/features/background/worker_pool.h"
#include "service/features/outbound_http/client.h"

namespace service::website_dns {

RUVIA_MODEL(dns_doh_answer_input, RUVIA_OPTIONAL_FIELD(type, ruvia::int64),
    RUVIA_OPTIONAL_FIELD(data, ruvia::string));
RUVIA_MODEL(dns_doh_envelope_input, RUVIA_OPTIONAL_FIELD_NAME("Status", status, ruvia::int64),
    RUVIA_OPTIONAL_FIELD_NAME("Answer", answer, ruvia::array<dns_doh_answer_input>));

struct probe_result final {
    bool matched_;
    std::string query_name_;
    std::string observed_target_;
};

inline std::string normalize_dns_name(std::string_view value) {
    while (!value.empty() && value.back() == '.') {
        value.remove_suffix(1);
    }
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return result;
}

inline std::string probe_name(std::string_view hostname, std::string_view website_domain_id) {
    if (!hostname.starts_with("*.")) {
        return std::string(hostname);
    }
    std::string token;
    token.reserve(8);
    for (const auto ch : website_domain_id) {
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

inline ruvia::task<probe_result> probe_cname(service::background::worker_context& context,
    std::string_view hostname,
    std::string_view expected_target,
    std::string_view website_domain_id) {
    const auto query_name = probe_name(hostname, website_domain_id);
    auto&& client = context.get_http_client(service::config::ali_dns_doh_origin_alias);
    std::array<ruvia::http_header_view, 1> headers{
        ruvia::http_header_view{"accept", "application/dns-json"}};
    const auto target = "/resolve?name=" + query_name + "&type=CNAME&cd=0&do=0";
    const auto response = co_await service::outbound_http::send_buffered(
        client,
        {
            .method_ = ruvia::known_http_method_token(ruvia::http_known_method::get),
            .target_ = target,
            .headers_ = headers,
            .content_ = ruvia::http_client_request_content_view::none(),
        },
        {.timeout_ = std::chrono::seconds(10), .stop_token_ = context.get_stop_token()});
    if (!response.status().is_successful()) {
        throw std::runtime_error("公共 DNS 探测服务返回异常");
    }
    const std::optional<dns_doh_envelope_input> parsed =
        ruvia::from_json<dns_doh_envelope_input>(response.body(), {.resource_ = context.pool()});
    if (!parsed || !parsed->get<"status">()) {
        throw std::runtime_error("公共 DNS 探测响应无效");
    }

    const auto expected = normalize_dns_name(expected_target);
    std::string observed;
    if (const auto& answers = parsed->get<"answer">(); answers) {
        for (const auto& answer : *answers) {
            const auto& type = answer.get<"type">();
            const auto& data = answer.get<"data">();
            if (!type || static_cast<std::int64_t>(*type) != 5 || !data) {
                continue;
            }
            const auto answer_target = normalize_dns_name(data->view());
            if (observed.empty()) {
                observed = answer_target;
            }
            if (answer_target == expected) {
                co_return probe_result{true, query_name, answer_target};
            }
        }
    }
    co_return probe_result{false, query_name, observed};
}

}  // namespace service::website_dns
