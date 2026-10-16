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
#include <ctime>
#include <iomanip>
#include <map>
#include <memory_resource>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

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

// Trace policies of the Alibaba Cloud DNS (2015-01-09) actions.
namespace aliyun_trace {

using namespace std::string_view_literals;
using service::attempt_trace::body_rule;
using service::attempt_trace::outbound_policy;

inline constexpr std::array request_id_headers{"x-acs-request-id"sv};
inline constexpr std::array error_code_fields{"Code"sv};
inline constexpr service::attempt_trace::reply_identity identity{.request_id_headers_ = request_id_headers,
    .request_id_field_ = "RequestId",
    .error_code_fields_ = error_code_fields};

// AccessKeyId, Signature and SignatureNonce are deliberately absent.
inline constexpr std::array query{"Action"sv, "DomainName"sv, "RR"sv, "Type"sv, "Value"sv, "TTL"sv, "Line"sv,
    "Priority"sv, "RecordId"sv, "PageNumber"sv, "PageSize"sv, "Format"sv, "Version"sv, "SignatureMethod"sv,
    "SignatureVersion"sv, "Timestamp"sv};
inline constexpr std::array response{"RequestId"sv, "Code"sv, "Message"sv, "HostId"sv, "Recommend"sv,
    "TotalCount"sv, "PageNumber"sv, "PageSize"sv, "RecordId"sv, "DomainRecords.Record[].RecordId"sv,
    "DomainRecords.Record[].RR"sv, "DomainRecords.Record[].Type"sv, "DomainRecords.Record[].Value"sv,
    "DomainRecords.Record[].TTL"sv, "DomainRecords.Record[].Line"sv, "DomainRecords.Record[].Status"sv,
    "DomainRecords.Record[].Locked"sv, "DomainRecords.Record[].Priority"sv, "DomainRecords.Record[].Weight"sv,
    "DomainRecords.Record[].DomainName"sv, "Domains.Domain[].DomainId"sv, "Domains.Domain[].DomainName"sv,
    "Domains.Domain[].PunyCode"sv, "Domains.Domain[].RecordCount"sv, "Domains.Domain[].DnsServers.DnsServer[]"sv,
    "RecordLines.RecordLine[].LineCode"sv, "RecordLines.RecordLine[].LineName"sv,
    "RecordLines.RecordLine[].FatherCode"sv, "RecordLines.RecordLine[].LineDisplayName"sv};

inline constexpr outbound_policy describe_domains{.code_ = "aliyun.describe_domains",
    .query_ = query,
    .response_body_ = body_rule::json,
    .response_fields_ = response,
    .identity_ = identity};
inline constexpr outbound_policy describe_domain_records{.code_ = "aliyun.describe_domain_records",
    .query_ = query,
    .response_body_ = body_rule::json,
    .response_fields_ = response,
    .identity_ = identity};
inline constexpr outbound_policy describe_support_lines{.code_ = "aliyun.describe_support_lines",
    .query_ = query,
    .response_body_ = body_rule::json,
    .response_fields_ = response,
    .identity_ = identity};
inline constexpr outbound_policy add_domain_record{.code_ = "aliyun.add_domain_record",
    .query_ = query,
    .response_body_ = body_rule::json,
    .response_fields_ = response,
    .identity_ = identity};
inline constexpr outbound_policy update_domain_record{.code_ = "aliyun.update_domain_record",
    .query_ = query,
    .response_body_ = body_rule::json,
    .response_fields_ = response,
    .identity_ = identity};
inline constexpr outbound_policy delete_domain_record{.code_ = "aliyun.delete_domain_record",
    .query_ = query,
    .response_body_ = body_rule::json,
    .response_fields_ = response,
    .identity_ = identity};

}  // namespace aliyun_trace

RUVIA_MODEL(aliyun_error_envelope, RUVIA_OPTIONAL_FIELD_NAME("Code", code, ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("Message", message, ruvia::string));

RUVIA_MODEL(aliyun_domain_payload,
    RUVIA_OPTIONAL_FIELD_NAME("DomainId", domain_id, ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("DomainName", domain_name, ruvia::string));
RUVIA_MODEL(aliyun_domain_list_payload,
    RUVIA_OPTIONAL_FIELD_NAME("Domain", domain, ruvia::array<aliyun_domain_payload>));
RUVIA_MODEL(aliyun_domain_list_envelope,
    RUVIA_OPTIONAL_FIELD_NAME("Domains", domains, aliyun_domain_list_payload),
    RUVIA_OPTIONAL_FIELD_NAME("TotalCount", total_count, ruvia::int64));

RUVIA_MODEL(aliyun_line_payload,
    RUVIA_OPTIONAL_FIELD_NAME("LineCode", line_code, ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("LineName", line_name, ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("LineDisplayName", line_display_name, ruvia::string));
RUVIA_MODEL(aliyun_line_list_payload,
    RUVIA_OPTIONAL_FIELD_NAME("RecordLine", record_line,
        ruvia::array<aliyun_line_payload>));
RUVIA_MODEL(aliyun_line_list_envelope,
    RUVIA_OPTIONAL_FIELD_NAME("RecordLines", record_lines, aliyun_line_list_payload));

RUVIA_MODEL(aliyun_record_payload,
    RUVIA_OPTIONAL_FIELD_NAME("RecordId", record_id, ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("RR", rr, ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("Type", type, ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("Value", value, ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("TTL", ttl, ruvia::int64),
    RUVIA_OPTIONAL_FIELD_NAME("Priority", priority, ruvia::int64),
    RUVIA_OPTIONAL_FIELD_NAME("Line", line, ruvia::string));
RUVIA_MODEL(aliyun_record_list_payload,
    RUVIA_OPTIONAL_FIELD_NAME("Record", record, ruvia::array<aliyun_record_payload>));
RUVIA_MODEL(aliyun_record_list_envelope,
    RUVIA_OPTIONAL_FIELD_NAME("DomainRecords", domain_records,
        aliyun_record_list_payload),
    RUVIA_OPTIONAL_FIELD_NAME("TotalCount", total_count, ruvia::int64));
RUVIA_MODEL(aliyun_record_id_envelope,
    RUVIA_OPTIONAL_FIELD_NAME("RecordId", record_id, ruvia::string));

namespace detail {

inline void append_aliyun_domains(const aliyun_domain_list_envelope& envelope,
    std::vector<provider_zone>& result) {
    const auto& domains = envelope.get<"domains">();
    if (!domains) {
        return;
    }
    const auto& domain_list = domains->get<"domain">();
    if (!domain_list) {
        return;
    }
    for (const auto& domain : *domain_list) {
        const auto& name = domain.get<"domain_name">();
        if (!name || name->view().empty()) {
            continue;
        }
        const auto& id = domain.get<"domain_id">();
        result.push_back(provider_zone{
            id && !id->view().empty() ? std::string(id->view()) : std::string(name->view()),
            std::string(name->view()),
            "active",
        });
    }
}

inline std::vector<provider_line> parse_aliyun_lines(const aliyun_line_list_envelope& envelope) {
    std::vector<provider_line> result;
    const auto& lines = envelope.get<"record_lines">();
    if (lines) {
        const auto& line_list = lines->get<"record_line">();
        if (line_list) {
            for (const auto& line : *line_list) {
                const auto& code = line.get<"line_code">();
                if (!code || code->view().empty()) {
                    continue;
                }
                const auto& name = line.get<"line_name">();
                const auto& display = line.get<"line_display_name">();
                auto resolved_name = name && !name->view().empty() ? std::string(name->view())
                                                                   : std::string(code->view());
                auto resolved_display = display && !display->view().empty()
                                            ? std::string(display->view())
                                            : resolved_name;
                result.push_back(provider_line{std::string(code->view()), std::move(resolved_name),
                    std::move(resolved_display)});
            }
        }
    }
    if (std::none_of(result.begin(), result.end(),
            [](const auto& line) { return line.code_ == "default"; })) {
        result.insert(result.begin(), provider_line{"default", "默认", "默认"});
    }
    return result;
}

inline void append_aliyun_records(const aliyun_record_list_envelope& envelope,
    std::vector<provider_record>& result) {
    const auto& records = envelope.get<"domain_records">();
    if (!records) {
        return;
    }
    const auto& record_list = records->get<"record">();
    if (!record_list) {
        return;
    }
    for (const auto& record : *record_list) {
        const auto& id = record.get<"record_id">();
        const auto& rr = record.get<"rr">();
        const auto& type = record.get<"type">();
        const auto& value = record.get<"value">();
        const auto& ttl = record.get<"ttl">();
        if (!id || !rr || !type || !value || !ttl) {
            continue;
        }
        provider_record item{.id_ = std::string(id->view()),
            .type_ = std::string(type->view()),
            .name_ = std::string(rr->view()),
            .content_ = std::string(value->view()),
            .ttl_ = static_cast<std::int64_t>(*ttl),
            .priority_ = std::nullopt,
            .proxied_ = false,
            .line_code_ = "default"};
        const auto& priority = record.get<"priority">();
        if (priority) {
            item.priority_ = static_cast<std::int64_t>(*priority);
        }
        const auto& line = record.get<"line">();
        if (line && !line->view().empty()) {
            item.line_code_ = std::string(line->view());
        }
        result.push_back(std::move(item));
    }
}

template <typename envelope_type>
inline std::int64_t aliyun_page_count(const envelope_type& envelope, std::size_t fallback) {
    const auto& total_count = envelope.template get<"total_count">();
    const auto total =
        total_count ? static_cast<std::int64_t>(*total_count) : static_cast<std::int64_t>(fallback);
    return std::max<std::int64_t>(1, (total + 99) / 100);
}

// The error of a failed Aliyun reply: `Code: Message` as the reply names
// them (a DomainRecordConflict as its Chinese reason), else the fallback. The text
// passes the attempt's redaction: a SignatureDoesNotMatch echo of the string to sign
// (AccessKeyId, SignatureNonce, Signature) and any credential of the attempt are
// replaced before the text can reach the task error or the server log.
[[nodiscard]] inline provider_error aliyun_reply_error(service::attempt_trace::recorder& trace,
    std::string_view body, provider_error_code fallback_code, std::string_view fallback_message,
    std::pmr::memory_resource* resource) {
    auto code = fallback_code;
    auto message = std::string(fallback_message);
    const std::optional<aliyun_error_envelope> parsed =
        ruvia::from_json<aliyun_error_envelope>(body, {.resource_ = resource});
    if (parsed) {
        const auto& remote_code = parsed->get<"code">();
        const auto& remote_message = parsed->get<"message">();
        const auto code_text = remote_code ? remote_code->view() : std::string_view{};
        const auto message_text = remote_message ? remote_message->view() : std::string_view{};
        if (!message_text.empty()) {
            message = code_text.empty() ? std::string(message_text)
                                        : std::string(code_text) + ": " + std::string(message_text);
        } else if (!code_text.empty()) {
            message += "（" + std::string(code_text) + "）";
        }
        if (code_text.find("InvalidAccessKeyId") != std::string_view::npos ||
            code_text.find("Signature") != std::string_view::npos) {
            code = provider_error_code::credential_invalid;
        } else if (code_text.find("Forbidden") != std::string_view::npos ||
                   code_text.find("Unauthorized") != std::string_view::npos) {
            code = provider_error_code::authorization_failed;
        } else if (code_text.find("DomainRecord") != std::string_view::npos &&
                   code_text.find("NotFound") != std::string_view::npos) {
            code = provider_error_code::record_not_found;
        } else if (code_text.find("Domain") != std::string_view::npos &&
                   code_text.find("NotFound") != std::string_view::npos) {
            code = provider_error_code::zone_not_found;
        } else if (code_text == "DomainRecordConflict") {
            code = provider_error_code::record_name_conflict;
            message = "阿里云 DNS 拒绝该记录：同名主机记录已有冲突的解析记录（DomainRecordConflict）";
        } else if (code_text.find("Duplicate") != std::string_view::npos ||
                   code_text.find("RecordForbidden") != std::string_view::npos) {
            code = provider_error_code::record_conflict;
        }
    }
    return provider_error(code, trace.redact_error(message));
}

}  // namespace detail

// Alibaba Cloud DNS. Records carry names relative to the zone ("@" for the
// apex) and a resolution line; the minimum TTL is 600 seconds and content is
// compared verbatim.
class aliyun_client final {
public:
    static constexpr std::string_view code{"aliyun"};
    static constexpr std::int64_t minimum_record_ttl{600};
    static constexpr record_name_policy record_names{false};

    explicit aliyun_client(service::outbound_http::quota_scope scope)
        : scope_(std::move(scope)) {}

    // The AccessKey must list the domains and their resolution lines.
    template <typename runtime_type>
    ruvia::task<std::vector<provider_zone>> verify(runtime_type& c, service::attempt_trace::recorder& trace,
        std::string_view access_key_id, std::string_view access_key_secret) const {
        auto domains = co_await list_domains(c, trace, access_key_id, access_key_secret);
        (void)co_await list_lines(c, trace, access_key_id, access_key_secret, {});
        co_return domains;
    }

    template <typename runtime_type>
    ruvia::task<provider_zone> find_zone(runtime_type& c, service::attempt_trace::recorder& trace,
        std::string_view access_key_id, std::string_view access_key_secret, std::string_view domain) const {
        const auto zones = co_await list_domains(c, trace, access_key_id, access_key_secret);
        const auto found = std::find_if(zones.begin(), zones.end(), [domain](const auto& zone) {
            return record_name_policy::dns_name_equals(zone.name_, domain);
        });
        if (found == zones.end()) {
            throw provider_error(provider_error_code::zone_not_found, "阿里云 DNS 中未找到该托管域名");
        }
        co_return *found;
    }

    template <typename runtime_type>
    ruvia::task<std::vector<provider_record>> list_records(runtime_type& c, service::attempt_trace::recorder& trace,
        std::string_view access_key_id, std::string_view access_key_secret, std::string_view,
        std::string_view domain_name) const {
        std::vector<provider_record> result;
        std::int64_t page = 1;
        std::int64_t total_pages = 1;
        do {
            std::map<std::string, std::string> params{
                {"DomainName", std::string(domain_name)},
                {"PageNumber", std::to_string(page)},
                {"PageSize", "100"},
            };
            const auto response = co_await send(c, trace, aliyun_trace::describe_domain_records,
                "DescribeDomainRecords", params, access_key_id, access_key_secret);
            const std::optional<aliyun_record_list_envelope> parsed =
                ruvia::from_json<aliyun_record_list_envelope>(response.body(), {.resource_ = c.pool()});
            if (!response.status().is_successful() || !parsed) {
                fail(c, trace, response.body(), provider_error_code::dns_failed, "阿里云 DNS 记录查询失败");
            }
            detail::append_aliyun_records(*parsed, result);
            total_pages = detail::aliyun_page_count(*parsed, result.size());
            ++page;
        } while (page <= total_pages);
        co_return result;
    }

    // The resolution lines of the domain, or of the account when `domain_name` is empty.
    template <typename runtime_type>
    ruvia::task<std::vector<provider_line>> list_lines(runtime_type& c, service::attempt_trace::recorder& trace,
        std::string_view access_key_id, std::string_view access_key_secret, std::string_view domain_name) const {
        std::map<std::string, std::string> params;
        if (!domain_name.empty()) {
            params.emplace("DomainName", std::string(domain_name));
        }
        const auto response = co_await send(c, trace, aliyun_trace::describe_support_lines, "DescribeSupportLines",
            params, access_key_id, access_key_secret);
        const std::optional<aliyun_line_list_envelope> parsed =
            ruvia::from_json<aliyun_line_list_envelope>(response.body(), {.resource_ = c.pool()});
        if (!response.status().is_successful() || !parsed) {
            fail(c, trace, response.body(), provider_error_code::upstream_failed, "阿里云 DNS 线路查询失败");
        }
        co_return detail::parse_aliyun_lines(*parsed);
    }

    template <typename runtime_type>
    ruvia::task<std::string> create_record(runtime_type& c, service::attempt_trace::recorder& trace,
        std::string_view access_key_id, std::string_view access_key_secret, std::string_view,
        std::string_view domain_name, const provider_record& desired) const {
        const auto params = record_params(domain_name, desired);
        const auto response = co_await send(c, trace, aliyun_trace::add_domain_record, "AddDomainRecord", params,
            access_key_id, access_key_secret, desired.name_ + " " + desired.type_);
        co_return record_id(c, trace, response, "阿里云 DNS 记录创建失败");
    }

    template <typename runtime_type>
    ruvia::task<std::string> update_record(runtime_type& c, service::attempt_trace::recorder& trace,
        std::string_view access_key_id, std::string_view access_key_secret, std::string_view,
        std::string_view domain_name, std::string_view record_id_value, const provider_record& desired) const {
        auto params = record_params(domain_name, desired);
        params.emplace("RecordId", std::string(record_id_value));
        const auto response = co_await send(c, trace, aliyun_trace::update_domain_record, "UpdateDomainRecord",
            params, access_key_id, access_key_secret, desired.name_ + " " + desired.type_);
        co_return record_id(c, trace, response, "阿里云 DNS 记录更新失败");
    }

    template <typename runtime_type>
    ruvia::task<void> delete_record(runtime_type& c, service::attempt_trace::recorder& trace,
        std::string_view access_key_id, std::string_view access_key_secret, std::string_view,
        std::string_view record_id_value, std::string_view subject) const {
        std::map<std::string, std::string> params{{"RecordId", std::string(record_id_value)}};
        try {
            const auto response = co_await send(c, trace, aliyun_trace::delete_domain_record, "DeleteDomainRecord",
                params, access_key_id, access_key_secret, subject);
            if (!response.status().is_successful()) {
                fail(c, trace, response.body(), provider_error_code::dns_failed, "阿里云 DNS 记录删除失败");
            }
        } catch (const provider_error& error) {
            const auto message = std::string_view(error.what());
            if (error.code() != provider_error_code::record_not_found &&
                message.find("does not exist") == std::string_view::npos) {
                throw;
            }
            trace.log(service::attempt_trace::log_level::warning, "服务商记录已不存在，视为已删除");
        }
        co_return;
    }

    // The same record as Aliyun compares it: type, name, content and line.
    [[nodiscard]] static bool same_identity(const provider_record& remote, const provider_record& desired) {
        return remote.type_ == desired.type_ && lower(remote.name_) == lower(desired.name_) &&
               remote.content_ == desired.content_ && remote.line_code_ == desired.line_code_;
    }

    // The record needs no update: same identity, TTL and MX priority.
    [[nodiscard]] static bool matches_desired(const provider_record& remote, const provider_record& desired) {
        if (!same_identity(remote, desired) || remote.ttl_ != desired.ttl_) {
            return false;
        }
        return desired.type_ != "MX" || remote.priority_ == desired.priority_;
    }

    [[noreturn]] static void throw_duplicate_records() {
        throw provider_error(provider_error_code::record_conflict, "阿里云 DNS 中存在多条相同线路解析记录，无法安全接管");
    }

private:
    template <typename runtime_type>
    ruvia::task<std::vector<provider_zone>> list_domains(runtime_type& c, service::attempt_trace::recorder& trace,
        std::string_view access_key_id, std::string_view access_key_secret) const {
        std::vector<provider_zone> result;
        std::int64_t page = 1;
        std::int64_t total_pages = 1;
        do {
            std::map<std::string, std::string> params{
                {"PageNumber", std::to_string(page)},
                {"PageSize", "100"},
            };
            const auto response = co_await send(c, trace, aliyun_trace::describe_domains, "DescribeDomains", params,
                access_key_id, access_key_secret);
            const std::optional<aliyun_domain_list_envelope> parsed =
                ruvia::from_json<aliyun_domain_list_envelope>(response.body(), {.resource_ = c.pool()});
            if (!response.status().is_successful() || !parsed) {
                fail(c, trace, response.body(), provider_error_code::upstream_failed, "阿里云 DNS 域名查询失败");
            }
            detail::append_aliyun_domains(*parsed, result);
            total_pages = detail::aliyun_page_count(*parsed, result.size());
            ++page;
        } while (page <= total_pages);
        co_return result;
    }

    template <typename runtime_type>
    [[nodiscard]] static std::string record_id(runtime_type& c, service::attempt_trace::recorder& trace,
        const service::outbound_http::buffered_response& response, std::string_view failure) {
        const std::optional<aliyun_record_id_envelope> parsed =
            ruvia::from_json<aliyun_record_id_envelope>(response.body(), {.resource_ = c.pool()});
        if (!response.status().is_successful() || !parsed || !parsed->get<"record_id">()) {
            fail(c, trace, response.body(), provider_error_code::dns_failed, failure);
        }
        return std::string(parsed->get<"record_id">()->view());
    }

    template <typename runtime_type>
    ruvia::task<service::outbound_http::buffered_response> send(runtime_type& c,
        service::attempt_trace::recorder& trace, const service::attempt_trace::outbound_policy& policy,
        std::string_view action, const std::map<std::string, std::string>& params, std::string_view access_key_id,
        std::string_view access_key_secret, std::string_view subject = {}) const {
        // The credentials of every request are the attempt's: whatever the
        // reply or an error echoes of them is redacted.
        trace.register_secret(access_key_id);
        trace.register_secret(access_key_secret);
        const auto target = "/?" + signed_query(action, params, access_key_id, access_key_secret);
        std::array<ruvia::http_header_view, 1> headers{
            ruvia::http_header_view{"accept", "application/json"},
        };
        co_return co_await service::outbound_http::send_buffered(c, scope_,
            {.origin_alias_ = service::config::aliyun_dns_origin_alias, .policy_ = &policy, .subject_ = subject},
            {.label_ = "阿里云 DNS API", .timeout_ = std::chrono::seconds(10)}, trace,
            {
                .method_ = ruvia::known_http_method_token(ruvia::http_known_method::get),
                .target_ = target,
                .headers_ = headers,
                .content_ = ruvia::http_client_request_content_view::none(),
            });
    }

    [[nodiscard]] static std::map<std::string, std::string> record_params(std::string_view domain_name,
        const provider_record& desired) {
        std::map<std::string, std::string> params{
            {"DomainName", std::string(domain_name)},
            {"RR", desired.name_},
            {"Type", desired.type_},
            {"Value", desired.content_},
            {"TTL", std::to_string(desired.ttl_)},
            {"Line", desired.line_code_.empty() ? std::string("default") : desired.line_code_},
        };
        if (desired.type_ == "MX" && desired.priority_) {
            params.emplace("Priority", std::to_string(*desired.priority_));
        }
        return params;
    }

    [[nodiscard]] static std::string signed_query(std::string_view action,
        const std::map<std::string, std::string>& params,
        std::string_view access_key_id,
        std::string_view access_key_secret) {
        std::map<std::string, std::string> all = params;
        all.emplace("Action", std::string(action));
        all.emplace("AccessKeyId", std::string(access_key_id));
        all.emplace("Format", "JSON");
        all.emplace("SignatureMethod", "HMAC-SHA1");
        all.emplace("SignatureNonce", nonce());
        all.emplace("SignatureVersion", "1.0");
        all.emplace("Timestamp", timestamp());
        all.emplace("Version", "2015-01-09");

        const auto canonical = canonical_query(all);
        const auto string_to_sign = "GET&%2F&" + service::utils::percent_encode(canonical);
        all.emplace("Signature", hmac_sha1_base64(access_key_secret, string_to_sign));
        return canonical_query(all);
    }

    [[nodiscard]] static std::string canonical_query(const std::map<std::string, std::string>& all) {
        std::string result;
        for (const auto& [key, value] : all) {
            if (!result.empty()) {
                result.push_back('&');
            }
            result += service::utils::percent_encode(key);
            result.push_back('=');
            result += service::utils::percent_encode(value);
        }
        return result;
    }

    [[nodiscard]] static std::string timestamp() {
        const auto now = std::chrono::system_clock::now();
        const auto raw = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
#ifdef _WIN32
        gmtime_s(&tm, &raw);
#else
        gmtime_r(&raw, &tm);
#endif
        std::ostringstream output;
        output << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
        return output.str();
    }

    [[nodiscard]] static std::string nonce() {
        std::array<unsigned char, 16> bytes{};
        const service::utils::sensitive_buffer_guard cleanse_bytes(bytes);
        if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
            throw provider_error(provider_error_code::upstream_failed, "阿里云 DNS 签名随机数生成失败");
        }
        constexpr char hex[] = "0123456789abcdef";
        std::string result;
        result.reserve(bytes.size() * 2);
        for (const auto byte : bytes) {
            result.push_back(hex[byte >> 4]);
            result.push_back(hex[byte & 0x0F]);
        }
        return result;
    }

    [[nodiscard]] static std::string hmac_sha1_base64(std::string_view secret,
        std::string_view payload) {
        service::utils::sensitive_string key(std::string(secret) + "&");
        std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
        const service::utils::sensitive_buffer_guard cleanse_digest(digest);
        unsigned int digest_size = 0;
        if (HMAC(EVP_sha1(), key.view().data(), static_cast<int>(key.view().size()),
                reinterpret_cast<const unsigned char*>(payload.data()), payload.size(),
                digest.data(), &digest_size) == nullptr) {
            throw provider_error(provider_error_code::upstream_failed, "阿里云 DNS 请求签名失败");
        }
        std::string output(4 * ((digest_size + 2) / 3), '\0');
        const auto size = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(output.data()),
            digest.data(), static_cast<int>(digest_size));
        if (size < 0) {
            throw provider_error(provider_error_code::upstream_failed, "阿里云 DNS 请求签名编码失败");
        }
        output.resize(static_cast<std::size_t>(size));
        return output;
    }

    [[nodiscard]] static std::string lower(std::string_view input) {
        std::string result(input);
        std::transform(result.begin(), result.end(), result.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return result;
    }

    template <typename runtime_type>
    [[noreturn]] static void fail(runtime_type& c, service::attempt_trace::recorder& trace, std::string_view body,
        provider_error_code fallback_code, std::string_view fallback_message) {
        throw detail::aliyun_reply_error(trace, body, fallback_code, fallback_message, c.pool());
    }

    service::outbound_http::quota_scope scope_;
};

}  // namespace service::dns
