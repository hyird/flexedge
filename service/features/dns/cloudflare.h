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
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <asio/ip/address.hpp>

#include "ruvia/web/model.h"

#include "service/config/outbound.h"
#include "service/features/outbound_http/client.h"
#include "service/utils/sensitive_string.h"

namespace service::dns {

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

enum class cloudflare_error_code {
    credential_invalid,
    authorization_failed,
    upstream_failed,
    zone_not_found,
    dns_failed,
    record_conflict,
    // The name already holds records the new one cannot share (81053, 81054, 81056).
    record_name_conflict,
    record_not_found,
};

class cloudflare_error final : public std::runtime_error {
public:
    cloudflare_error(cloudflare_error_code code, std::string_view message)
        : std::runtime_error(std::string(message)),
          code_(code) {}

    [[nodiscard]] cloudflare_error_code code() const noexcept {
        return code_;
    }

private:
    cloudflare_error_code code_;
};

struct cloudflare_zone {
    std::string id_;
    std::string name_;
    std::string status_;
};

struct cloudflare_record {
    std::string id_;
    std::string type_;
    std::string name_;
    std::string content_;
    std::int64_t ttl_;
    std::optional<std::int64_t> priority_;
    bool proxied_;
    std::string modified_at_;
};

struct cloudflare_record_page {
    std::vector<cloudflare_record> list_;
    std::int64_t total_;
    std::int64_t total_pages_;
};

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
[[nodiscard]] cloudflare_error cloudflare_reply_error(service::attempt_trace::recorder& trace,
    const std::optional<envelope_type>& envelope, cloudflare_error_code code, std::string_view fallback) {
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
                resolved_code = cloudflare_error_code::authorization_failed;
            } else if (value == cloudflare_record_already_exists_code) {
                resolved_code = cloudflare_error_code::record_conflict;
            } else if (name_conflict) {
                resolved_code = cloudflare_error_code::record_name_conflict;
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
    return cloudflare_error(resolved_code, trace.redact_error(message));
}

}  // namespace detail

class cloudflare_client final {
public:
    template <typename runtime_type>
    ruvia::task<std::vector<cloudflare_zone>> verify_token(runtime_type& c, service::attempt_trace::recorder& trace,
        std::string_view token, std::string_view account_id) const {
        trace.register_secret(account_id);
        const auto response = co_await send(c, trace, service::attempt_trace::outbound_operation::cloudflare_verify_token,
            ruvia::http_known_method::get, "/client/v4/accounts/" + std::string(account_id) + "/tokens/verify", token);
        const std::optional<cloudflare_token_envelope> parsed =
            ruvia::from_json<cloudflare_token_envelope>(response.body(), {.resource_ = c.pool()});
        if (!response.status().is_successful() || !parsed || !success(*parsed)) {
            fail(trace, parsed, cloudflare_error_code::credential_invalid,
                "Cloudflare API Token 无效或未激活");
        }
        const auto& result = parsed->get<"result">();
        if (!result) {
            throw cloudflare_error(cloudflare_error_code::credential_invalid,
                "Cloudflare API Token 无效或未激活");
        }
        const auto& status = result->get<"status">();
        if (!status || status->view() != "active") {
            throw cloudflare_error(cloudflare_error_code::credential_invalid,
                "Cloudflare API Token 无效或未激活");
        }

        co_return co_await list_zones(c, trace, token, account_id);
    }

    template <typename runtime_type>
    ruvia::task<std::vector<cloudflare_zone>> list_zones(runtime_type& c, service::attempt_trace::recorder& trace,
        std::string_view token, std::string_view account_id) const {
        trace.register_secret(account_id);
        std::vector<cloudflare_zone> result;
        std::int64_t page = 1;
        std::int64_t total_pages = 1;
        do {
            const auto target =
                "/client/v4/zones?account.id=" + percent_encode(account_id) +
                "&status=active&order=name&direction=asc&per_page=50&page=" + std::to_string(page);
            const auto response = co_await send(c, trace, service::attempt_trace::outbound_operation::cloudflare_list_zones,
                ruvia::http_known_method::get, target, token);
            const std::optional<cloudflare_zone_envelope> parsed =
                ruvia::from_json<cloudflare_zone_envelope>(response.body(),
                    {.resource_ = c.pool()});
            if (!response.status().is_successful() || !parsed || !success(*parsed)) {
                fail(trace, parsed, cloudflare_error_code::credential_invalid,
                    "Cloudflare API Token 缺少 Zone Read 权限");
            }
            const auto& zones = parsed->get<"result">();
            if (zones) {
                for (const auto& zone : *zones) {
                    const auto& id = zone.get<"id">();
                    const auto& name = zone.get<"name">();
                    const auto& status = zone.get<"status">();
                    if (id && name && status) {
                        result.push_back(cloudflare_zone{std::string(id->view()),
                            std::string(name->view()),
                            std::string(status->view())});
                    }
                }
            }
            total_pages = 1;
            const auto& info = parsed->get<"result_info">();
            if (info) {
                const auto& value = info->get<"total_pages">();
                if (value) {
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

    template <typename runtime_type>
    ruvia::task<cloudflare_zone> find_zone(runtime_type& c, service::attempt_trace::recorder& trace,
        std::string_view token, std::string_view account_id, std::string_view domain) const {
        trace.register_secret(account_id);
        std::string candidate = normalize_domain(domain);
        if (candidate.starts_with("*.")) {
            candidate.erase(0, 2);
        }

        while (candidate.find('.') != std::string::npos) {
            const auto target = "/client/v4/zones?account.id=" + percent_encode(account_id) +
                                "&name=" + percent_encode(candidate) + "&status=active&per_page=5";
            const auto response = co_await send(c, trace, service::attempt_trace::outbound_operation::cloudflare_find_zone,
                ruvia::http_known_method::get, target, token, std::nullopt, candidate);
            const std::optional<cloudflare_zone_envelope> parsed =
                ruvia::from_json<cloudflare_zone_envelope>(response.body(),
                    {.resource_ = c.pool()});
            if (!response.status().is_successful() || !parsed || !success(*parsed)) {
                fail(trace, parsed, cloudflare_error_code::upstream_failed, "Cloudflare Zone 查询失败");
            }

            const auto& zones = parsed->get<"result">();
            if (zones) {
                for (const auto& zone : *zones) {
                    const auto& id = zone.get<"id">();
                    const auto& name = zone.get<"name">();
                    const auto& status = zone.get<"status">();
                    if (id && name && status && status->view() == "active" &&
                        name->view() == candidate) {
                        co_return cloudflare_zone{std::string(id->view()), std::string(name->view()),
                            std::string(status->view())};
                    }
                }
            }
            candidate.erase(0, candidate.find('.') + 1);
        }
        throw cloudflare_error(cloudflare_error_code::zone_not_found,
            "Cloudflare 中未找到该域名对应的活动 Zone");
    }

    template <typename runtime_type>
    ruvia::task<cloudflare_record_page>
    list_records(runtime_type& c, service::attempt_trace::recorder& trace, std::string_view token,
        std::string_view zone_id, std::int64_t page, std::int64_t page_size,
        const std::optional<std::string>& keyword) const {
        auto target = "/client/v4/zones/" + std::string(zone_id) +
                      "/dns_records?order=name&direction=asc&page=" + std::to_string(page) +
                      "&per_page=" + std::to_string(page_size);
        if (keyword && !keyword->empty()) {
            target += "&search=" + percent_encode(*keyword);
        }
        const auto response = co_await send(c, trace, service::attempt_trace::outbound_operation::cloudflare_list_records,
            ruvia::http_known_method::get, target, token);
        const std::optional<cloudflare_record_list_envelope> parsed =
            ruvia::from_json<cloudflare_record_list_envelope>(response.body(),
                {.resource_ = c.pool()});
        if (!response.status().is_successful() || !parsed || !success(*parsed)) {
            fail(trace, parsed, cloudflare_error_code::dns_failed, "Cloudflare DNS 记录查询失败");
        }

        cloudflare_record_page result;
        const auto& records = parsed->get<"result">();
        if (records) {
            result.list_.reserve(records->size());
            for (const auto& record : *records) {
                const auto& id = record.get<"id">();
                const auto& type = record.get<"type">();
                const auto& name = record.get<"name">();
                const auto& content = record.get<"content">();
                const auto& ttl = record.get<"ttl">();
                if (!id || !type || !name || !content || !ttl) {
                    continue;
                }
                cloudflare_record item{std::string(id->view()),
                    std::string(type->view()),
                    std::string(name->view()),
                    std::string(content->view()),
                    *ttl,
                    std::nullopt,
                    false,
                    {}};
                const auto& priority = record.get<"priority">();
                if (priority) {
                    item.priority_ = *priority;
                }
                const auto& proxied_value = record.get<"proxied">();
                if (proxied_value) {
                    item.proxied_ = static_cast<bool>(*proxied_value);
                }
                const auto& modified_on = record.get<"modified_on">();
                if (modified_on) {
                    item.modified_at_ = std::string(modified_on->view());
                }
                result.list_.push_back(std::move(item));
            }
        }
        const auto& info = parsed->get<"result_info">();
        result.total_ = static_cast<std::int64_t>(result.list_.size());
        result.total_pages_ = 1;
        if (info) {
            const auto& total_count = info->get<"total_count">();
            if (total_count) {
                result.total_ = static_cast<std::int64_t>(*total_count);
            }
            const auto& total_pages = info->get<"total_pages">();
            if (total_pages) {
                result.total_pages_ = static_cast<std::int64_t>(*total_pages);
            }
        }
        co_return result;
    }

    template <typename runtime_type>
    ruvia::task<std::vector<cloudflare_record>> list_all_records(runtime_type& c,
        service::attempt_trace::recorder& trace, std::string_view token, std::string_view zone_id) const {
        std::vector<cloudflare_record> result;
        std::int64_t page = 1;
        std::int64_t total_pages = 1;
        do {
            auto current = co_await list_records(c, trace, token, zone_id, page, 100, std::nullopt);
            result.insert(result.end(), std::make_move_iterator(current.list_.begin()),
                std::make_move_iterator(current.list_.end()));
            total_pages = current.total_pages_;
            ++page;
        } while (page <= total_pages);
        co_return result;
    }

    template <typename runtime_type>
    ruvia::task<std::string>
    create_or_adopt_record(runtime_type& c, service::attempt_trace::recorder& trace, std::string_view token,
        std::string_view zone_id, std::string_view type, std::string_view name, std::string_view content,
        std::int64_t ttl, std::optional<std::int64_t> priority, bool proxied) const {
        auto existing_id = co_await find_exact_record_id(c, trace, token, zone_id, type, name, content);
        if (existing_id) {
            trace.log(service::attempt_trace::log_level::info, "服务商已有相同记录，改为更新该记录");
            co_return co_await update_record(c, trace, token, zone_id, *existing_id, type, name, content, ttl,
                priority, proxied);
        }

        bool duplicate = false;
        try {
            co_return co_await save_record(c, trace, service::attempt_trace::outbound_operation::cloudflare_create_record,
                ruvia::http_known_method::post,
                "/client/v4/zones/" + std::string(zone_id) +
                    "/dns_records",
                token, type, name, content, ttl, priority, proxied);
        } catch (const cloudflare_error& error) {
            if (error.code() != cloudflare_error_code::record_conflict) {
                throw;
            }
            duplicate = true;
        }

        if (duplicate) {
            trace.log(service::attempt_trace::log_level::warning, "服务商报告记录已存在（81057），查找后接管该记录");
            existing_id = co_await find_exact_record_id(c, trace, token, zone_id, type, name, content);
            if (existing_id) {
                co_return co_await update_record(c, trace, token, zone_id, *existing_id, type, name, content,
                    ttl, priority, proxied);
            }
        }
        throw cloudflare_error(cloudflare_error_code::record_conflict,
            "Cloudflare DNS 记录冲突，未找到可安全接管的相同记录");
    }

    template <typename runtime_type>
    ruvia::task<std::string>
    update_record(runtime_type& c, service::attempt_trace::recorder& trace, std::string_view token,
        std::string_view zone_id, std::string_view record_id, std::string_view type, std::string_view name,
        std::string_view content, std::int64_t ttl, std::optional<std::int64_t> priority,
        bool proxied) const {
        co_return co_await save_record(c, trace, service::attempt_trace::outbound_operation::cloudflare_update_record,
            ruvia::http_known_method::put,
            "/client/v4/zones/" + std::string(zone_id) + "/dns_records/" +
                std::string(record_id),
            token, type, name, content, ttl, priority, proxied);
    }

    template <typename runtime_type>
    ruvia::task<void> delete_record(runtime_type& c, service::attempt_trace::recorder& trace, std::string_view token,
        std::string_view zone_id, std::string_view record_id, std::string_view subject) const {
        const auto target =
            "/client/v4/zones/" + std::string(zone_id) + "/dns_records/" + std::string(record_id);
        const auto response = co_await send(c, trace, service::attempt_trace::outbound_operation::cloudflare_delete_record,
            ruvia::http_known_method::delete_value, target, token, std::nullopt, subject);
        if (response.status() == ruvia::http_status::not_found) {
            trace.log(service::attempt_trace::log_level::warning, "服务商记录已不存在（404），视为已删除");
            co_return;
        }
        const std::optional<cloudflare_delete_envelope> parsed =
            ruvia::from_json<cloudflare_delete_envelope>(response.body(), {.resource_ = c.pool()});
        if (!response.status().is_successful() || !parsed || !success(*parsed)) {
            fail(trace, parsed, cloudflare_error_code::dns_failed, "Cloudflare DNS 记录删除失败");
        }
        co_return;
    }

private:
    template <typename runtime_type>
    ruvia::task<service::outbound_http::buffered_response>
    send(runtime_type& c, service::attempt_trace::recorder& trace, service::attempt_trace::outbound_operation operation,
        ruvia::http_known_method method, std::string_view target, std::string_view token,
        std::optional<std::string_view> body = std::nullopt, std::string_view subject = {}) const {
        // The token of every request is the attempt's credential: whatever the
        // reply or an error echoes of it is redacted.
        trace.register_secret(token);
        auto&& client = c.get_http_client(service::config::cloudflare_origin_alias);
        service::utils::sensitive_string authorization("Bearer " + std::string(token));
        std::array<ruvia::http_header_view, 3> headers{
            ruvia::http_header_view{"authorization", authorization.view()},
            ruvia::http_header_view{"accept", "application/json"},
            ruvia::http_header_view{"content-type", "application/json"},
        };
        const auto header_count = body ? headers.size() : headers.size() - 1;
        try {
            co_return co_await service::outbound_http::send_buffered(
                client, {.origin_alias_ = service::config::cloudflare_origin_alias, .operation_ = operation, .subject_ = subject},
                trace,
                {
                    .method_ = ruvia::known_http_method_token(method),
                    .target_ = target,
                    .headers_ = std::span(headers).first(header_count),
                    .content_ = body ? ruvia::http_client_request_content_view::bytes(*body)
                                     : ruvia::http_client_request_content_view::none(),
                },
                {.timeout_ = std::chrono::seconds(10), .stop_token_ = c.get_stop_token()});
        } catch (const ruvia::http_client_error& error) {
            using code = ruvia::http_client_error::code_type;
            switch (error.code()) {
                case code::timeout:
                    throw cloudflare_error(cloudflare_error_code::upstream_failed,
                        "Cloudflare API 请求超时（10 秒）");
                case code::resolve_failed:
                    throw cloudflare_error(cloudflare_error_code::upstream_failed,
                        "Cloudflare API 域名解析失败");
                case code::connect_failed:
                    throw cloudflare_error(cloudflare_error_code::upstream_failed,
                        "Cloudflare API 连接失败");
                case code::tls_failed:
                    throw cloudflare_error(cloudflare_error_code::upstream_failed,
                        "Cloudflare API TLS 连接失败");
                case code::response_too_large:
                    throw cloudflare_error(cloudflare_error_code::upstream_failed,
                        "Cloudflare API 响应超过大小限制");
                default:
                    throw cloudflare_error(cloudflare_error_code::upstream_failed,
                        "Cloudflare API 请求失败");
            }
        }
    }

    template <typename runtime_type>
    ruvia::task<std::string> save_record(runtime_type& c, service::attempt_trace::recorder& trace,
        service::attempt_trace::outbound_operation operation, ruvia::http_known_method method, std::string target,
        std::string_view token, std::string_view type, std::string_view name, std::string_view content,
        std::int64_t ttl, std::optional<std::int64_t> priority, bool proxied) const {
        cloudflare_save_record_payload payload({.resource_ = c.pool()});
        payload.set<"type">(type);
        payload.set<"name">(name);
        payload.set<"content">(content);
        payload.set<"ttl">(ttl);
        if (type == "MX" && priority) {
            payload.set<"priority">(*priority);
        }
        if (type == "A" || type == "AAAA" || type == "CNAME") {
            payload.set<"proxied">(proxied);
        }
        const auto json = ruvia::to_json(payload, {.resource_ = c.pool()});
        const auto subject = std::string(name) + " " + std::string(type);
        const auto response = co_await send(c, trace, operation, method, target, token, json, subject);
        if (response.status() == ruvia::http_status::not_found) {
            throw cloudflare_error(cloudflare_error_code::record_not_found, "Cloudflare DNS 记录不存在");
        }
        const std::optional<cloudflare_record_envelope> parsed =
            ruvia::from_json<cloudflare_record_envelope>(response.body(), {.resource_ = c.pool()});
        if (!response.status().is_successful() || !parsed || !success(*parsed)) {
            if (parsed) {
                fail(trace, parsed, cloudflare_error_code::dns_failed, "Cloudflare DNS 记录保存失败");
            }
            const std::optional<cloudflare_error_envelope> error_envelope =
                ruvia::from_json<cloudflare_error_envelope>(response.body(),
                    {.resource_ = c.pool()});
            fail(trace, error_envelope, cloudflare_error_code::dns_failed, "Cloudflare DNS 记录保存失败");
        }
        const auto& result = parsed->get<"result">();
        if (!result) {
            throw cloudflare_error(cloudflare_error_code::dns_failed,
                "Cloudflare DNS 记录保存响应缺少记录 ID");
        }
        const auto& id = result->get<"id">();
        if (!id) {
            throw cloudflare_error(cloudflare_error_code::dns_failed,
                "Cloudflare DNS 记录保存响应缺少记录 ID");
        }
        co_return std::string(id->view());
    }

    template <typename runtime_type>
    ruvia::task<std::optional<std::string>>
    find_exact_record_id(runtime_type& c, service::attempt_trace::recorder& trace, std::string_view token,
        std::string_view zone_id, std::string_view type, std::string_view name,
        std::string_view content) const {
        const auto target = "/client/v4/zones/" + std::string(zone_id) +
                            "/dns_records?match=all&type=" + percent_encode(type) +
                            "&name.exact=" + percent_encode(name) +
                            "&content.exact=" + percent_encode(content) + "&per_page=100";
        const auto subject = std::string(name) + " " + std::string(type);
        const auto response = co_await send(c, trace, service::attempt_trace::outbound_operation::cloudflare_find_record,
            ruvia::http_known_method::get, target, token, std::nullopt, subject);
        const std::optional<cloudflare_record_list_envelope> parsed =
            ruvia::from_json<cloudflare_record_list_envelope>(response.body(),
                {.resource_ = c.pool()});
        if (!response.status().is_successful() || !parsed || !success(*parsed)) {
            fail(trace, parsed, cloudflare_error_code::dns_failed, "Cloudflare DNS 相同记录查询失败");
        }

        std::optional<std::string> result;
        const auto& records = parsed->get<"result">();
        if (!records) {
            co_return result;
        }
        for (const auto& record : *records) {
            const auto& id = record.get<"id">();
            const auto& record_type = record.get<"type">();
            const auto& record_name = record.get<"name">();
            const auto& record_content = record.get<"content">();
            if (!id || !record_type || !record_name || !record_content || record_type->view() != type ||
                !dns_name_equals(record_name->view(), name) ||
                !record_content_equals(type, record_content->view(), content)) {
                continue;
            }
            if (result && *result != id->view()) {
                throw cloudflare_error(cloudflare_error_code::record_conflict,
                    "Cloudflare 中存在多条相同 DNS 记录，无法安全接管");
            }
            result = std::string(id->view());
        }
        co_return result;
    }

    template <typename envelope_type>
    [[nodiscard]] static bool success(const envelope_type& envelope) {
        const auto& value = envelope.template get<"success">();
        return value && static_cast<bool>(*value);
    }

    template <typename envelope_type>
    [[noreturn]] static void fail(service::attempt_trace::recorder& trace, const std::optional<envelope_type>& envelope,
        cloudflare_error_code code, std::string_view fallback) {
        throw detail::cloudflare_reply_error(trace, envelope, code, fallback);
    }

    [[nodiscard]] static std::string normalize_domain(std::string_view input) {
        std::string result(input);
        std::transform(result.begin(), result.end(), result.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return result;
    }

    [[nodiscard]] static bool dns_name_equals(std::string_view left, std::string_view right) {
        auto normalized_left = normalize_domain(left);
        auto normalized_right = normalize_domain(right);
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

    [[nodiscard]] static bool record_content_equals(std::string_view type, std::string_view left,
        std::string_view right) {
        if (type == "A" || type == "AAAA") {
            return ip_address_equals(left, right);
        }
        if (type == "CNAME" || type == "MX") {
            return dns_name_equals(left, right);
        }
        return left == right;
    }

    [[nodiscard]] static std::string percent_encode(std::string_view input) {
        constexpr char hex[] = "0123456789ABCDEF";
        std::string result;
        result.reserve(input.size());
        for (const unsigned char ch : input) {
            if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
                ch == '-' || ch == '.' || ch == '_' || ch == '~') {
                result.push_back(static_cast<char>(ch));
            } else {
                result.push_back('%');
                result.push_back(hex[ch >> 4]);
                result.push_back(hex[ch & 0x0F]);
            }
        }
        return result;
    }
};

inline const cloudflare_client& get_cloudflare_client() {
    static const cloudflare_client client;
    return client;
}

}  // namespace service::dns
