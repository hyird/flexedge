#pragma once

#include <ruvia/http/http_known_method.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/web/model.h"

#include "service/config/outbound.h"
#include "service/features/attempt_trace/outbound_policy.h"
#include "service/features/background/task_context.h"
#include "service/features/outbound_http/client.h"

namespace service::website_dns {

// Trace policy of a DNS over HTTPS (JSON API) query.
namespace doh_trace {

using namespace std::string_view_literals;

inline constexpr std::array request_id_headers{"x-request-id"sv};
inline constexpr std::array query{"name"sv, "type"sv, "cd"sv, "do"sv};
inline constexpr std::array response{"Status"sv, "TC"sv, "RD"sv, "RA"sv, "AD"sv, "CD"sv, "Question[].name"sv,
    "Question[].type"sv, "Answer[].name"sv, "Answer[].type"sv, "Answer[].TTL"sv, "Answer[].data"sv,
    "Authority[].name"sv, "Authority[].type"sv, "Authority[].TTL"sv, "Authority[].data"sv, "Comment"sv};

inline constexpr service::attempt_trace::outbound_policy resolve{.code_ = "doh.resolve",
    .query_ = query,
    .response_body_ = service::attempt_trace::body_rule::json,
    .response_fields_ = response,
    .identity_ = {.request_id_headers_ = request_id_headers},
    .dns_query_ = true};

}  // namespace doh_trace

RUVIA_MODEL(dns_doh_answer_input, RUVIA_OPTIONAL_FIELD(type, ruvia::int64),
    RUVIA_OPTIONAL_FIELD(data, ruvia::string));
RUVIA_MODEL(dns_doh_envelope_input, RUVIA_OPTIONAL_FIELD_NAME("Status", status, ruvia::int64),
    RUVIA_OPTIONAL_FIELD_NAME("Answer", answer, ruvia::array<dns_doh_answer_input>));

struct probe_result final {
    bool matched_;
    std::string query_name_;
    std::string observed_target_;
    // Every CNAME target in the answer, normalized, in answer order.
    std::vector<std::string> answers_;
};

inline constexpr std::int64_t dns_type_a{1};
inline constexpr std::int64_t dns_type_cname{5};
inline constexpr std::int64_t dns_type_aaaa{28};

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

// The answer data of `answer_type` records for one DoH query of `query_name`.
inline ruvia::task<std::vector<std::string>> resolve_answers(service::background::task_context& context,
    service::attempt_trace::recorder& trace, std::string_view hostname, std::string_view query_name,
    std::string_view query_type, std::int64_t answer_type, const service::outbound_http::quota_scope& scope) {
    std::array<ruvia::http_header_view, 1> headers{
        ruvia::http_header_view{"accept", "application/dns-json"}};
    const auto target = "/resolve?name=" + std::string(query_name) + "&type=" + std::string(query_type) +
                        "&cd=0&do=0";
    const auto response = co_await service::outbound_http::send_buffered(context, scope,
        {.origin_alias_ = service::config::ali_dns_doh_origin_alias, .policy_ = &doh_trace::resolve, .subject_ = hostname},
        {.label_ = "公共 DNS 探测服务", .timeout_ = std::chrono::seconds(10)}, trace,
        {
            .method_ = ruvia::known_http_method_token(ruvia::http_known_method::get),
            .target_ = target,
            .headers_ = headers,
            .content_ = ruvia::http_client_request_content_view::none(),
        });
    if (!response.status().is_successful()) {
        throw std::runtime_error("公共 DNS 探测服务返回异常");
    }
    const std::optional<dns_doh_envelope_input> parsed =
        ruvia::from_json<dns_doh_envelope_input>(response.body(), {.resource_ = context.pool()});
    if (!parsed || !parsed->get<"status">()) {
        trace.mark_invalid_response("公共 DNS 探测响应无效");
        throw std::runtime_error("公共 DNS 探测响应无效");
    }
    std::vector<std::string> result;
    if (const auto& answers = parsed->get<"answer">(); answers) {
        for (const auto& answer : *answers) {
            const auto& type = answer.get<"type">();
            const auto& data = answer.get<"data">();
            if (type && static_cast<std::int64_t>(*type) == answer_type && data) {
                result.push_back(normalize_dns_name(data->view()));
            }
        }
    }
    co_return result;
}

inline ruvia::task<probe_result> probe_cname(service::background::task_context& context,
    service::attempt_trace::recorder& trace, std::string_view hostname, std::string_view expected_target,
    std::string_view website_domain_id, const service::outbound_http::quota_scope& scope) {
    auto query_name = probe_name(hostname, website_domain_id);
    auto answers = co_await resolve_answers(context, trace, hostname, query_name, "CNAME", dns_type_cname, scope);
    const auto expected = normalize_dns_name(expected_target);
    probe_result result{.matched_ = false,
        .query_name_ = std::move(query_name),
        .observed_target_ = answers.empty() ? std::string{} : answers.front(),
        .answers_ = std::move(answers)};
    if (std::ranges::find(result.answers_, expected) != result.answers_.end()) {
        result.matched_ = true;
        result.observed_target_ = expected;
    }
    co_return result;
}

// The A and AAAA answers for a name whose CNAME query came back empty.
inline ruvia::task<std::vector<std::string>> probe_addresses(service::background::task_context& context,
    service::attempt_trace::recorder& trace, std::string_view hostname, std::string_view query_name,
    const service::outbound_http::quota_scope& scope) {
    auto result = co_await resolve_answers(context, trace, hostname, query_name, "A", dns_type_a, scope);
    auto ipv6 = co_await resolve_answers(context, trace, hostname, query_name, "AAAA", dns_type_aaaa, scope);
    result.insert(result.end(), std::make_move_iterator(ipv6.begin()), std::make_move_iterator(ipv6.end()));
    co_return result;
}

}  // namespace service::website_dns
