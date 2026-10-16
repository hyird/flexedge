#pragma once

#include <ruvia/core/task.h>
#include <ruvia/http/http_known_method.h>
#include <ruvia/web/context.h>
#include <ruvia/web/http_client_handle.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <asio/ip/address.hpp>

#include "ruvia/web/model.h"

#include "service/config/outbound.h"
#include "service/features/attempt_trace/outbound_policy.h"
#include "service/features/dns/provider_error.h"
#include "service/features/dns/record_model.h"
#include "service/features/dns/record_name_policy.h"
#include "service/features/outbound_http/client.h"
#include "service/utils/percent_encoding.h"
#include "service/utils/sensitive_string.h"

namespace service::dns {

// Trace policies of the Cloudflare API v4 operations.
namespace cloudflare_trace {

using namespace std::string_view_literals;
using service::attempt_trace::body_rule;
using service::attempt_trace::outbound_policy;

inline constexpr std::array request_id_headers{"cf-ray"sv};
inline constexpr std::array error_code_fields{"errors[].code"sv};
inline constexpr service::attempt_trace::reply_identity identity{
    .request_id_headers_ = request_id_headers, .error_code_fields_ = error_code_fields};

// The account id in a path is masked; it is never on a query allow-list.
inline constexpr std::array accounts_path{"accounts"sv};
inline constexpr std::array zone_query{"name"sv, "status"sv, "order"sv, "direction"sv, "page"sv, "per_page"sv};
inline constexpr std::array record_query{"order"sv, "direction"sv, "page"sv, "per_page"sv, "match"sv, "type"sv,
    "name.exact"sv, "content.exact"sv, "search"sv};
inline constexpr std::array token_response{"success"sv, "errors[].code"sv, "errors[].message"sv,
    "messages[].code"sv, "messages[].message"sv, "result.status"sv, "result.expires_on"sv, "result.not_before"sv};
inline constexpr std::array zone_response{"success"sv, "errors[].code"sv, "errors[].message"sv, "messages[].code"sv,
    "messages[].message"sv, "result_info.page"sv, "result_info.per_page"sv, "result_info.count"sv,
    "result_info.total_count"sv, "result_info.total_pages"sv, "result[].id"sv, "result[].name"sv,
    "result[].status"sv, "result[].paused"sv, "result[].type"sv, "result[].name_servers[]"sv};
inline constexpr std::array record_list_response{"success"sv, "errors[].code"sv, "errors[].message"sv,
    "messages[].code"sv, "messages[].message"sv, "result_info.page"sv, "result_info.per_page"sv,
    "result_info.count"sv, "result_info.total_count"sv, "result_info.total_pages"sv, "result[].id"sv,
    "result[].type"sv, "result[].name"sv, "result[].content"sv, "result[].ttl"sv, "result[].proxied"sv,
    "result[].priority"sv, "result[].zone_name"sv, "result[].created_on"sv, "result[].modified_on"sv};
inline constexpr std::array record_response{"success"sv, "errors[].code"sv, "errors[].message"sv,
    "messages[].code"sv, "messages[].message"sv, "result.id"sv, "result.type"sv, "result.name"sv,
    "result.content"sv, "result.ttl"sv, "result.proxied"sv, "result.priority"sv, "result.modified_on"sv};
inline constexpr std::array record_request{"type"sv, "name"sv, "content"sv, "ttl"sv, "priority"sv, "proxied"sv};

inline constexpr outbound_policy verify_token{.code_ = "cloudflare.verify_token",
    .masked_path_after_ = accounts_path,
    .response_body_ = body_rule::json,
    .response_fields_ = token_response,
    .identity_ = identity};
inline constexpr outbound_policy list_zones{.code_ = "cloudflare.list_zones",
    .query_ = zone_query,
    .response_body_ = body_rule::json,
    .response_fields_ = zone_response,
    .identity_ = identity};
inline constexpr outbound_policy find_zone{.code_ = "cloudflare.find_zone",
    .query_ = zone_query,
    .response_body_ = body_rule::json,
    .response_fields_ = zone_response,
    .identity_ = identity};
inline constexpr outbound_policy list_records{.code_ = "cloudflare.list_records",
    .query_ = record_query,
    .response_body_ = body_rule::json,
    .response_fields_ = record_list_response,
    .identity_ = identity};
inline constexpr outbound_policy find_record{.code_ = "cloudflare.find_record",
    .query_ = record_query,
    .response_body_ = body_rule::json,
    .response_fields_ = record_list_response,
    .identity_ = identity};
inline constexpr outbound_policy create_record{.code_ = "cloudflare.create_record",
    .request_body_ = body_rule::json,
    .request_fields_ = record_request,
    .response_body_ = body_rule::json,
    .response_fields_ = record_response,
    .identity_ = identity};
inline constexpr outbound_policy update_record{.code_ = "cloudflare.update_record",
    .request_body_ = body_rule::json,
    .request_fields_ = record_request,
    .response_body_ = body_rule::json,
    .response_fields_ = record_response,
    .identity_ = identity};
inline constexpr outbound_policy delete_record{.code_ = "cloudflare.delete_record",
    .response_body_ = body_rule::json,
    .response_fields_ = record_response,
    .identity_ = identity};

}  // namespace cloudflare_trace

RUVIA_MODEL(cloudflare_api_error_payload, RUVIA_OPTIONAL_FIELD(code, ruvia::int64),
    RUVIA_OPTIONAL_FIELD(message, ruvia::string));

RUVIA_MODEL(cloudflare_error_envelope, RUVIA_OPTIONAL_FIELD(success, ruvia::bool_value),
    RUVIA_OPTIONAL_FIELD(errors, ruvia::array<cloudflare_api_error_payload>));

RUVIA_MODEL(cloudflare_token_payload, RUVIA_OPTIONAL_FIELD(id, ruvia::string),
    RUVIA_OPTIONAL_FIELD(status, ruvia::string));

RUVIA_MODEL(cloudflare_token_envelope, RUVIA_OPTIONAL_FIELD(success, ruvia::bool_value),
    RUVIA_OPTIONAL_FIELD(result, cloudflare_token_payload),
    RUVIA_OPTIONAL_FIELD(errors, ruvia::array<cloudflare_api_error_payload>));

RUVIA_MODEL(cloudflare_zone_payload, RUVIA_OPTIONAL_FIELD(id, ruvia::string),
    RUVIA_OPTIONAL_FIELD(name, ruvia::string),
    RUVIA_OPTIONAL_FIELD(status, ruvia::string));

RUVIA_MODEL(cloudflare_result_info, RUVIA_OPTIONAL_FIELD(page, ruvia::int64),
    RUVIA_OPTIONAL_FIELD_NAME("per_page", per_page, ruvia::int64),
    RUVIA_OPTIONAL_FIELD_NAME("total_count", total_count, ruvia::int64),
    RUVIA_OPTIONAL_FIELD_NAME("total_pages", total_pages, ruvia::int64));

RUVIA_MODEL(cloudflare_zone_envelope, RUVIA_OPTIONAL_FIELD(success, ruvia::bool_value),
    RUVIA_OPTIONAL_FIELD(result, ruvia::array<cloudflare_zone_payload>),
    RUVIA_OPTIONAL_FIELD_NAME("result_info", result_info, cloudflare_result_info),
    RUVIA_OPTIONAL_FIELD(errors, ruvia::array<cloudflare_api_error_payload>));

RUVIA_MODEL(cloudflare_record_payload, RUVIA_OPTIONAL_FIELD(id, ruvia::string),
    RUVIA_OPTIONAL_FIELD(type, ruvia::string),
    RUVIA_OPTIONAL_FIELD(name, ruvia::string),
    RUVIA_OPTIONAL_FIELD(content, ruvia::string),
    RUVIA_OPTIONAL_FIELD(ttl, ruvia::int64),
    RUVIA_OPTIONAL_FIELD(priority, ruvia::int64),
    RUVIA_OPTIONAL_FIELD(proxied, ruvia::bool_value),
    RUVIA_OPTIONAL_FIELD_NAME("modified_on", modified_on, ruvia::string));

RUVIA_MODEL(cloudflare_record_envelope, RUVIA_OPTIONAL_FIELD(success, ruvia::bool_value),
    RUVIA_OPTIONAL_FIELD(result, cloudflare_record_payload),
    RUVIA_OPTIONAL_FIELD(errors, ruvia::array<cloudflare_api_error_payload>));

RUVIA_MODEL(cloudflare_record_list_envelope, RUVIA_OPTIONAL_FIELD(success, ruvia::bool_value),
    RUVIA_OPTIONAL_FIELD(result, ruvia::array<cloudflare_record_payload>),
    RUVIA_OPTIONAL_FIELD_NAME("result_info", result_info, cloudflare_result_info),
    RUVIA_OPTIONAL_FIELD(errors, ruvia::array<cloudflare_api_error_payload>));

RUVIA_MODEL(cloudflare_delete_envelope, RUVIA_OPTIONAL_FIELD(success, ruvia::bool_value),
    RUVIA_OPTIONAL_FIELD(errors, ruvia::array<cloudflare_api_error_payload>));

RUVIA_MODEL(cloudflare_save_record_payload, RUVIA_REQUIRED_FIELD(type, ruvia::string),
    RUVIA_REQUIRED_FIELD(name, ruvia::string),
    RUVIA_REQUIRED_FIELD(content, ruvia::string),
    RUVIA_REQUIRED_FIELD(ttl, ruvia::int64),
    RUVIA_OPTIONAL_FIELD(priority, ruvia::int64, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD(proxied, ruvia::bool_value, RUVIA_OMIT_EMPTY));

namespace detail {

inline constexpr std::int64_t cloudflare_record_already_exists_code{81057};

// Cloudflare's rejection of a record whose name already holds records it cannot share.
[[nodiscard]] inline std::optional<std::string_view> cloudflare_name_conflict_message(std::int64_t code) {
    switch (code) {
        case 81053:
            return "Cloudflare 拒绝该记录：同名主机记录已有 A、AAAA 或 CNAME 记录（81053）";
        case 81054:
            return "Cloudflare 拒绝该记录：同名主机记录已有 CNAME 记录（81054）";
        case 81056:
            return "Cloudflare 拒绝该记录：同名主机记录已有 NS 记录（81056）";
        default:
            return std::nullopt;
    }
}

// The error of a failed Cloudflare reply: the first error's message (code 81057 as the
// duplicate-record message, a name conflict as its Chinese reason), else the fallback; codes
// 10000, 81057 and the name conflicts refine the error code. The text passes the attempt's
// redaction, so a token the API echoes (e.g. in an `Authorization: Bearer …` string) or any
// other credential of the attempt never reaches the task error or the log.
template <typename envelope_type>
[[nodiscard]] provider_error cloudflare_reply_error(service::attempt_trace::recorder& trace,
    const std::optional<envelope_type>& envelope, provider_error_code code, std::string_view fallback) {
    auto resolved_code = code;
    std::string message(fallback);
    if (envelope) {
        const auto& errors = envelope->template get<"errors">();
        if (errors && !errors->empty()) {
            const auto& first = errors->front();
            const auto& api_code = first.template get<"code">();
            const auto value = api_code ? static_cast<std::int64_t>(*api_code) : std::int64_t{0};
            const auto& text = first.template get<"message">();
            const auto name_conflict = cloudflare_name_conflict_message(value);
            if (value == 10000) {
                resolved_code = provider_error_code::authorization_failed;
            } else if (value == cloudflare_record_already_exists_code) {
                resolved_code = provider_error_code::record_conflict;
            } else if (name_conflict) {
                resolved_code = provider_error_code::record_name_conflict;
            }
            if (value == cloudflare_record_already_exists_code) {
                message = "Cloudflare 中已存在相同 DNS 记录";
            } else if (name_conflict) {
                message = *name_conflict;
            } else if (text && !text->view().empty()) {
                message = std::string(text->view());
            }
        }
    }
    return provider_error(resolved_code, trace.redact_error(message));
}

}  // namespace detail

// Cloudflare API v4. Records carry fully qualified names; A, AAAA and CNAME
// records may be proxied; content is compared as Cloudflare normalizes it.
class cloudflare_client final {
public:
    static constexpr std::string_view code{"cloudflare"};
    static constexpr std::int64_t minimum_record_ttl{1};
    static constexpr record_name_policy record_names{true};

    explicit cloudflare_client(service::outbound_http::quota_scope scope)
        : scope_(std::move(scope)) {}

    // The token must be active and able to read the account's zones.
    template <typename runtime_type>
    ruvia::task<std::vector<provider_zone>> verify(runtime_type& c, service::attempt_trace::recorder& trace,
        std::string_view account_id, std::string_view token) const {
        trace.register_secret(account_id);
        const auto response = co_await send(c, trace, cloudflare_trace::verify_token, ruvia::http_known_method::get,
            "/client/v4/accounts/" + std::string(account_id) + "/tokens/verify", token);
        const std::optional<cloudflare_token_envelope> parsed =
            ruvia::from_json<cloudflare_token_envelope>(response.body(), {.resource_ = c.pool()});
        if (!response.status().is_successful() || !parsed || !success(*parsed)) {
            fail(trace, parsed, provider_error_code::credential_invalid, "Cloudflare API Token 无效或未激活");
        }
        const auto& result = parsed->get<"result">();
        if (!result || !result->get<"status">() || result->get<"status">()->view() != "active") {
            throw provider_error(provider_error_code::credential_invalid, "Cloudflare API Token 无效或未激活");
        }
        co_return co_await list_zones(c, trace, token, account_id);
    }

    template <typename runtime_type>
    ruvia::task<provider_zone> find_zone(runtime_type& c, service::attempt_trace::recorder& trace,
        std::string_view account_id, std::string_view token, std::string_view domain) const {
        trace.register_secret(account_id);
        std::string candidate = lower(domain);
        if (candidate.starts_with("*.")) {
            candidate.erase(0, 2);
        }
        while (candidate.find('.') != std::string::npos) {
            const auto target = "/client/v4/zones?account.id=" + service::utils::percent_encode(account_id) +
                                "&name=" + service::utils::percent_encode(candidate) + "&status=active&per_page=5";
            const auto response = co_await send(c, trace, cloudflare_trace::find_zone, ruvia::http_known_method::get,
                target, token, std::nullopt, candidate);
            const std::optional<cloudflare_zone_envelope> parsed =
                ruvia::from_json<cloudflare_zone_envelope>(response.body(), {.resource_ = c.pool()});
            if (!response.status().is_successful() || !parsed || !success(*parsed)) {
                fail(trace, parsed, provider_error_code::upstream_failed, "Cloudflare Zone 查询失败");
            }
            if (const auto& zones = parsed->get<"result">(); zones) {
                for (const auto& zone : *zones) {
                    const auto& id = zone.get<"id">();
                    const auto& name = zone.get<"name">();
                    const auto& status = zone.get<"status">();
                    if (id && name && status && status->view() == "active" && name->view() == candidate) {
                        co_return provider_zone{std::string(id->view()), std::string(name->view()),
                            std::string(status->view())};
                    }
                }
            }
            candidate.erase(0, candidate.find('.') + 1);
        }
        throw provider_error(provider_error_code::zone_not_found, "Cloudflare 中未找到该域名对应的活动 Zone");
    }

    template <typename runtime_type>
    ruvia::task<std::vector<provider_record>> list_records(runtime_type& c, service::attempt_trace::recorder& trace,
        std::string_view, std::string_view token, std::string_view zone_id, std::string_view) const {
        std::vector<provider_record> result;
        std::int64_t page = 1;
        std::int64_t total_pages = 1;
        do {
            total_pages = co_await list_record_page(c, trace, token, zone_id, page, result);
            ++page;
        } while (page <= total_pages);
        co_return result;
    }

    // Cloudflare has only the default line.
    template <typename runtime_type>
    ruvia::task<std::vector<provider_line>> list_lines(runtime_type&, service::attempt_trace::recorder&,
        std::string_view, std::string_view, std::string_view) const {
        co_return std::vector<provider_line>{{"default", "默认", "默认"}};
    }

    // Creates the record, or takes over an identical one the zone already has.
    template <typename runtime_type>
    ruvia::task<std::string> create_record(runtime_type& c, service::attempt_trace::recorder& trace,
        std::string_view, std::string_view token, std::string_view zone_id, std::string_view,
        const provider_record& desired) const {
        auto existing_id = co_await find_exact_record_id(c, trace, token, zone_id, desired);
        if (existing_id) {
            trace.log(service::attempt_trace::log_level::info, "服务商已有相同记录，改为更新该记录");
            co_return co_await save_record(c, trace, cloudflare_trace::update_record, ruvia::http_known_method::put,
                record_target(zone_id, *existing_id), token, desired);
        }
        bool duplicate = false;
        try {
            co_return co_await save_record(c, trace, cloudflare_trace::create_record, ruvia::http_known_method::post,
                "/client/v4/zones/" + std::string(zone_id) + "/dns_records", token, desired);
        } catch (const provider_error& error) {
            if (error.code() != provider_error_code::record_conflict) {
                throw;
            }
            duplicate = true;
        }
        if (duplicate) {
            trace.log(service::attempt_trace::log_level::warning, "服务商报告记录已存在（81057），查找后接管该记录");
            existing_id = co_await find_exact_record_id(c, trace, token, zone_id, desired);
            if (existing_id) {
                co_return co_await save_record(c, trace, cloudflare_trace::update_record,
                    ruvia::http_known_method::put, record_target(zone_id, *existing_id), token, desired);
            }
        }
        throw provider_error(provider_error_code::record_conflict, "Cloudflare DNS 记录冲突，未找到可安全接管的相同记录");
    }

    template <typename runtime_type>
    ruvia::task<std::string> update_record(runtime_type& c, service::attempt_trace::recorder& trace,
        std::string_view, std::string_view token, std::string_view zone_id, std::string_view,
        std::string_view record_id, const provider_record& desired) const {
        co_return co_await save_record(c, trace, cloudflare_trace::update_record, ruvia::http_known_method::put,
            record_target(zone_id, record_id), token, desired);
    }

    template <typename runtime_type>
    ruvia::task<void> delete_record(runtime_type& c, service::attempt_trace::recorder& trace, std::string_view,
        std::string_view token, std::string_view zone_id, std::string_view record_id,
        std::string_view subject) const {
        const auto response = co_await send(c, trace, cloudflare_trace::delete_record,
            ruvia::http_known_method::delete_value, record_target(zone_id, record_id), token, std::nullopt, subject);
        if (response.status() == ruvia::http_status::not_found) {
            trace.log(service::attempt_trace::log_level::warning, "服务商记录已不存在（404），视为已删除");
            co_return;
        }
        const std::optional<cloudflare_delete_envelope> parsed =
            ruvia::from_json<cloudflare_delete_envelope>(response.body(), {.resource_ = c.pool()});
        if (!response.status().is_successful() || !parsed || !success(*parsed)) {
            fail(trace, parsed, provider_error_code::dns_failed, "Cloudflare DNS 记录删除失败");
        }
        co_return;
    }

    // The same record as Cloudflare compares it: type, name and content.
    [[nodiscard]] static bool same_identity(const provider_record& remote, const provider_record& desired) {
        return remote.type_ == desired.type_ && dns_name_equals(remote.name_, desired.name_) &&
               content_equals(desired.type_, remote.content_, desired.content_);
    }

    // The record needs no update: same identity, TTL, MX priority and, for
    // proxiable types, proxy state.
    [[nodiscard]] static bool matches_desired(const provider_record& remote, const provider_record& desired) {
        if (!same_identity(remote, desired) || remote.ttl_ != desired.ttl_) {
            return false;
        }
        if (desired.type_ == "MX" && remote.priority_ != desired.priority_) {
            return false;
        }
        return !proxiable(desired.type_) || remote.proxied_ == desired.proxied_;
    }

    [[noreturn]] static void throw_duplicate_records() {
        throw provider_error(provider_error_code::record_conflict, "Cloudflare 中存在多条相同 DNS 记录，无法安全接管");
    }

private:
    template <typename runtime_type>
    ruvia::task<std::vector<provider_zone>> list_zones(runtime_type& c, service::attempt_trace::recorder& trace,
        std::string_view token, std::string_view account_id) const {
        std::vector<provider_zone> result;
        std::int64_t page = 1;
        std::int64_t total_pages = 1;
        do {
            const auto target = "/client/v4/zones?account.id=" + service::utils::percent_encode(account_id) +
                                "&status=active&order=name&direction=asc&per_page=50&page=" + std::to_string(page);
            const auto response =
                co_await send(c, trace, cloudflare_trace::list_zones, ruvia::http_known_method::get, target, token);
            const std::optional<cloudflare_zone_envelope> parsed =
                ruvia::from_json<cloudflare_zone_envelope>(response.body(), {.resource_ = c.pool()});
            if (!response.status().is_successful() || !parsed || !success(*parsed)) {
                fail(trace, parsed, provider_error_code::credential_invalid, "Cloudflare API Token 缺少 Zone Read 权限");
            }
            if (const auto& zones = parsed->get<"result">(); zones) {
                for (const auto& zone : *zones) {
                    const auto& id = zone.get<"id">();
                    const auto& name = zone.get<"name">();
                    const auto& status = zone.get<"status">();
                    if (id && name && status) {
                        result.push_back(provider_zone{std::string(id->view()), std::string(name->view()),
                            std::string(status->view())});
                    }
                }
            }
            total_pages = 1;
            if (const auto& info = parsed->get<"result_info">(); info) {
                if (const auto& value = info->get<"total_pages">(); value) {
                    total_pages = static_cast<std::int64_t>(*value);
                }
            }
            ++page;
            if (page <= total_pages) {
                trace.log(service::attempt_trace::log_level::info,
                    "已读取第 " + std::to_string(page - 1) + " 页，共 " + std::to_string(total_pages) + " 页托管域");
            }
        } while (page <= total_pages);
        co_return result;
    }

    // Appends one page of records; returns the page count.
    template <typename runtime_type>
    ruvia::task<std::int64_t> list_record_page(runtime_type& c, service::attempt_trace::recorder& trace,
        std::string_view token, std::string_view zone_id, std::int64_t page,
        std::vector<provider_record>& result) const {
        const auto target = "/client/v4/zones/" + std::string(zone_id) +
                            "/dns_records?order=name&direction=asc&page=" + std::to_string(page) + "&per_page=100";
        const auto response =
            co_await send(c, trace, cloudflare_trace::list_records, ruvia::http_known_method::get, target, token);
        const std::optional<cloudflare_record_list_envelope> parsed =
            ruvia::from_json<cloudflare_record_list_envelope>(response.body(), {.resource_ = c.pool()});
        if (!response.status().is_successful() || !parsed || !success(*parsed)) {
            fail(trace, parsed, provider_error_code::dns_failed, "Cloudflare DNS 记录查询失败");
        }
        if (const auto& records = parsed->get<"result">(); records) {
            for (const auto& record : *records) {
                const auto& id = record.get<"id">();
                const auto& type = record.get<"type">();
                const auto& name = record.get<"name">();
                const auto& content = record.get<"content">();
                const auto& ttl = record.get<"ttl">();
                if (!id || !type || !name || !content || !ttl) {
                    continue;
                }
                const auto& priority = record.get<"priority">();
                const auto& proxied = record.get<"proxied">();
                result.push_back(provider_record{.id_ = std::string(id->view()),
                    .type_ = std::string(type->view()),
                    .name_ = std::string(name->view()),
                    .content_ = std::string(content->view()),
                    .ttl_ = *ttl,
                    .priority_ = priority ? std::optional<std::int64_t>(*priority) : std::nullopt,
                    .proxied_ = proxied && static_cast<bool>(*proxied),
                    .line_code_ = "default"});
            }
        }
        std::int64_t total_pages = 1;
        if (const auto& info = parsed->get<"result_info">(); info) {
            if (const auto& value = info->get<"total_pages">(); value) {
                total_pages = static_cast<std::int64_t>(*value);
            }
        }
        co_return total_pages;
    }

    template <typename runtime_type>
    ruvia::task<service::outbound_http::buffered_response> send(runtime_type& c,
        service::attempt_trace::recorder& trace, const service::attempt_trace::outbound_policy& policy,
        ruvia::http_known_method method, std::string_view target, std::string_view token,
        std::optional<std::string_view> body = std::nullopt, std::string_view subject = {}) const {
        // The token of every request is the attempt's credential: whatever the
        // reply or an error echoes of it is redacted.
        trace.register_secret(token);
        service::utils::sensitive_string authorization("Bearer " + std::string(token));
        std::array<ruvia::http_header_view, 3> headers{
            ruvia::http_header_view{"authorization", authorization.view()},
            ruvia::http_header_view{"accept", "application/json"},
            ruvia::http_header_view{"content-type", "application/json"},
        };
        const auto header_count = body ? headers.size() : headers.size() - 1;
        co_return co_await service::outbound_http::send_buffered(c, scope_,
            {.origin_alias_ = service::config::cloudflare_origin_alias, .policy_ = &policy, .subject_ = subject},
            {.label_ = "Cloudflare API", .timeout_ = std::chrono::seconds(10)}, trace,
            {
                .method_ = ruvia::known_http_method_token(method),
                .target_ = target,
                .headers_ = std::span(headers).first(header_count),
                .content_ = body ? ruvia::http_client_request_content_view::bytes(*body)
                                 : ruvia::http_client_request_content_view::none(),
            });
    }

    template <typename runtime_type>
    ruvia::task<std::string> save_record(runtime_type& c, service::attempt_trace::recorder& trace,
        const service::attempt_trace::outbound_policy& policy, ruvia::http_known_method method, std::string target,
        std::string_view token, const provider_record& desired) const {
        cloudflare_save_record_payload payload({.resource_ = c.pool()});
        payload.set<"type">(desired.type_);
        payload.set<"name">(desired.name_);
        payload.set<"content">(desired.content_);
        payload.set<"ttl">(desired.ttl_);
        if (desired.type_ == "MX" && desired.priority_) {
            payload.set<"priority">(*desired.priority_);
        }
        if (proxiable(desired.type_)) {
            payload.set<"proxied">(desired.proxied_);
        }
        const auto json = ruvia::to_json(payload, {.resource_ = c.pool()});
        const auto subject = desired.name_ + " " + desired.type_;
        const auto response = co_await send(c, trace, policy, method, target, token, json, subject);
        if (response.status() == ruvia::http_status::not_found) {
            throw provider_error(provider_error_code::record_not_found, "Cloudflare DNS 记录不存在");
        }
        const std::optional<cloudflare_record_envelope> parsed =
            ruvia::from_json<cloudflare_record_envelope>(response.body(), {.resource_ = c.pool()});
        if (!response.status().is_successful() || !parsed || !success(*parsed)) {
            if (parsed) {
                fail(trace, parsed, provider_error_code::dns_failed, "Cloudflare DNS 记录保存失败");
            }
            const std::optional<cloudflare_error_envelope> error_envelope =
                ruvia::from_json<cloudflare_error_envelope>(response.body(), {.resource_ = c.pool()});
            fail(trace, error_envelope, provider_error_code::dns_failed, "Cloudflare DNS 记录保存失败");
        }
        const auto& result = parsed->get<"result">();
        if (!result || !result->get<"id">()) {
            throw provider_error(provider_error_code::dns_failed, "Cloudflare DNS 记录保存响应缺少记录 ID");
        }
        const auto& id = result->get<"id">();
        co_return std::string(id->view());
    }

    template <typename runtime_type>
    ruvia::task<std::optional<std::string>> find_exact_record_id(runtime_type& c,
        service::attempt_trace::recorder& trace, std::string_view token, std::string_view zone_id,
        const provider_record& desired) const {
        const auto target = "/client/v4/zones/" + std::string(zone_id) +
                            "/dns_records?match=all&type=" + service::utils::percent_encode(desired.type_) +
                            "&name.exact=" + service::utils::percent_encode(desired.name_) +
                            "&content.exact=" + service::utils::percent_encode(desired.content_) + "&per_page=100";
        const auto subject = desired.name_ + " " + desired.type_;
        const auto response = co_await send(c, trace, cloudflare_trace::find_record, ruvia::http_known_method::get,
            target, token, std::nullopt, subject);
        const std::optional<cloudflare_record_list_envelope> parsed =
            ruvia::from_json<cloudflare_record_list_envelope>(response.body(), {.resource_ = c.pool()});
        if (!response.status().is_successful() || !parsed || !success(*parsed)) {
            fail(trace, parsed, provider_error_code::dns_failed, "Cloudflare DNS 相同记录查询失败");
        }
        std::optional<std::string> result;
        const auto& records = parsed->get<"result">();
        if (!records) {
            co_return result;
        }
        for (const auto& record : *records) {
            const auto& id = record.get<"id">();
            const auto& type = record.get<"type">();
            const auto& name = record.get<"name">();
            const auto& content = record.get<"content">();
            if (!id || !type || !name || !content || type->view() != desired.type_ ||
                !dns_name_equals(name->view(), desired.name_) ||
                !content_equals(desired.type_, content->view(), desired.content_)) {
                continue;
            }
            if (result && *result != id->view()) {
                throw_duplicate_records();
            }
            result = std::string(id->view());
        }
        co_return result;
    }

    [[nodiscard]] static std::string record_target(std::string_view zone_id, std::string_view record_id) {
        return "/client/v4/zones/" + std::string(zone_id) + "/dns_records/" + std::string(record_id);
    }

    [[nodiscard]] static bool proxiable(std::string_view type) {
        return type == "A" || type == "AAAA" || type == "CNAME";
    }

    template <typename envelope_type>
    [[nodiscard]] static bool success(const envelope_type& envelope) {
        const auto& value = envelope.template get<"success">();
        return value && static_cast<bool>(*value);
    }

    template <typename envelope_type>
    [[noreturn]] static void fail(service::attempt_trace::recorder& trace, const std::optional<envelope_type>& envelope,
        provider_error_code code, std::string_view fallback) {
        throw detail::cloudflare_reply_error(trace, envelope, code, fallback);
    }

    [[nodiscard]] static std::string lower(std::string_view input) {
        std::string result(input);
        std::transform(result.begin(), result.end(), result.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return result;
    }

    [[nodiscard]] static bool dns_name_equals(std::string_view left, std::string_view right) {
        auto normalized_left = lower(left);
        auto normalized_right = lower(right);
        if (normalized_left.ends_with('.')) {
            normalized_left.pop_back();
        }
        if (normalized_right.ends_with('.')) {
            normalized_right.pop_back();
        }
        return normalized_left == normalized_right;
    }

    [[nodiscard]] static bool ip_address_equals(std::string_view left, std::string_view right) {
        std::error_code left_error;
        std::error_code right_error;
        const auto left_address = asio::ip::make_address(left, left_error);
        const auto right_address = asio::ip::make_address(right, right_error);
        return !left_error && !right_error && left_address == right_address;
    }

    [[nodiscard]] static bool content_equals(std::string_view type, std::string_view left, std::string_view right) {
        if (type == "A" || type == "AAAA") {
            return ip_address_equals(left, right);
        }
        if (type == "CNAME" || type == "MX") {
            return dns_name_equals(left, right);
        }
        return left == right;
    }

    service::outbound_http::quota_scope scope_;
};

}  // namespace service::dns
