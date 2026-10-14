#pragma once

#include <ruvia/core/task.h>
#include <ruvia/http/http_known_method.h>
#include <ruvia/web/context.h>
#include <ruvia/web/http_client_handle.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <map>
#include <memory_resource>
#include <optional>
#include <sstream>
#include <stdexcept>
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
#include "service/features/outbound_http/client.h"
#include "service/utils/sensitive_string.h"

namespace service::dns {

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

enum class aliyun_error_code {
    credential_invalid,
    authorization_failed,
    upstream_failed,
    domain_not_found,
    dns_failed,
    record_conflict,
    record_not_found,
};

class aliyun_error final : public std::runtime_error {
public:
    aliyun_error(aliyun_error_code code, std::string_view message)
        : std::runtime_error(std::string(message)),
          code_(code) {}

    [[nodiscard]] aliyun_error_code code() const noexcept {
        return code_;
    }

private:
    aliyun_error_code code_;
};

struct aliyun_domain {
    std::string id_;
    std::string name_;
    std::string status_;
};

struct aliyun_line {
    std::string code_;
    std::string name_;
    std::string display_name_;
};

struct aliyun_record {
    std::string id_;
    std::string rr_;
    std::string type_;
    std::string value_;
    std::int64_t ttl_;
    std::optional<std::int64_t> priority_;
    std::string line_;
};

namespace detail {

inline void append_aliyun_domains(const aliyun_domain_list_envelope& envelope,
    std::vector<aliyun_domain>& result) {
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
        result.push_back(aliyun_domain{
            id && !id->view().empty() ? std::string(id->view()) : std::string(name->view()),
            std::string(name->view()),
            "active",
        });
    }
}

inline std::vector<aliyun_line> parse_aliyun_lines(const aliyun_line_list_envelope& envelope) {
    std::vector<aliyun_line> result;
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
                result.push_back(aliyun_line{std::string(code->view()), std::move(resolved_name),
                    std::move(resolved_display)});
            }
        }
    }
    if (std::none_of(result.begin(), result.end(),
            [](const auto& line) { return line.code_ == "default"; })) {
        result.insert(result.begin(), aliyun_line{"default", "默认", "默认"});
    }
    return result;
}

inline void append_aliyun_records(const aliyun_record_list_envelope& envelope,
    std::vector<aliyun_record>& result) {
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
        aliyun_record item{std::string(id->view()),
            std::string(rr->view()),
            std::string(type->view()),
            std::string(value->view()),
            static_cast<std::int64_t>(*ttl),
            std::nullopt,
            "default"};
        const auto& priority = record.get<"priority">();
        if (priority) {
            item.priority_ = static_cast<std::int64_t>(*priority);
        }
        const auto& line = record.get<"line">();
        if (line && !line->view().empty()) {
            item.line_ = std::string(line->view());
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
// them, else the fallback. The text passes the attempt's redaction: a
// SignatureDoesNotMatch echo of the string to sign (AccessKeyId,
// SignatureNonce, Signature) and any credential of the attempt are replaced
// before the text can reach the task error or the server log.
[[nodiscard]] inline aliyun_error aliyun_reply_error(service::attempt_trace::recorder& trace, std::string_view body,
    aliyun_error_code fallback_code, std::string_view fallback_message, std::pmr::memory_resource* resource) {
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
            code = aliyun_error_code::credential_invalid;
        } else if (code_text.find("Forbidden") != std::string_view::npos ||
                   code_text.find("Unauthorized") != std::string_view::npos) {
            code = aliyun_error_code::authorization_failed;
        } else if (code_text.find("DomainRecord") != std::string_view::npos &&
                   code_text.find("NotFound") != std::string_view::npos) {
            code = aliyun_error_code::record_not_found;
        } else if (code_text.find("Domain") != std::string_view::npos &&
                   code_text.find("NotFound") != std::string_view::npos) {
            code = aliyun_error_code::domain_not_found;
        } else if (code_text.find("Duplicate") != std::string_view::npos ||
                   code_text.find("RecordForbidden") != std::string_view::npos) {
            code = aliyun_error_code::record_conflict;
        }
    }
    return aliyun_error(code, trace.redact_error(message));
}

}  // namespace detail

class aliyun_client final {
public:
    template <typename runtime_type>
    ruvia::task<std::vector<aliyun_domain>> verify_access_key(runtime_type& c, service::attempt_trace::recorder& trace,
        std::string_view access_key_id, std::string_view access_key_secret) const {
        auto domains = co_await list_domains(c, trace, access_key_id, access_key_secret);
        (void)co_await list_support_lines(c, trace, access_key_id, access_key_secret, std::nullopt);
        co_return domains;
    }

    template <typename runtime_type>
    ruvia::task<std::vector<aliyun_domain>> list_domains(runtime_type& c, service::attempt_trace::recorder& trace,
        std::string_view access_key_id, std::string_view access_key_secret) const {
        std::vector<aliyun_domain> result;
        std::int64_t page = 1;
        std::int64_t total_pages = 1;
        do {
            std::map<std::string, std::string> params{
                {"PageNumber", std::to_string(page)},
                {"PageSize", "100"},
            };
            const auto response = co_await send(c, trace, service::attempt_trace::outbound_operation::aliyun_describe_domains,
                "DescribeDomains", params, access_key_id, access_key_secret);
            const std::optional<aliyun_domain_list_envelope> parsed =
                ruvia::from_json<aliyun_domain_list_envelope>(response.body(),
                    {.resource_ = c.pool()});
            if (!response.status().is_successful() || !parsed) {
                fail(c, trace, response.body(), aliyun_error_code::upstream_failed,
                    "阿里云 DNS 域名查询失败");
            }
            detail::append_aliyun_domains(*parsed, result);
            total_pages = detail::aliyun_page_count(*parsed, result.size());
            ++page;
        } while (page <= total_pages);
        co_return result;
    }

    template <typename runtime_type>
    ruvia::task<std::vector<aliyun_line>>
    list_support_lines(runtime_type& c, service::attempt_trace::recorder& trace, std::string_view access_key_id,
        std::string_view access_key_secret, std::optional<std::string_view> domain_name) const {
        std::map<std::string, std::string> params;
        if (domain_name && !domain_name->empty()) {
            params.emplace("DomainName", std::string(*domain_name));
        }
        const auto response = co_await send(c, trace,
            service::attempt_trace::outbound_operation::aliyun_describe_support_lines, "DescribeSupportLines", params,
            access_key_id, access_key_secret);
        const std::optional<aliyun_line_list_envelope> parsed =
            ruvia::from_json<aliyun_line_list_envelope>(response.body(), {.resource_ = c.pool()});
        if (!response.status().is_successful() || !parsed) {
            fail(c, trace, response.body(), aliyun_error_code::upstream_failed, "阿里云 DNS 线路查询失败");
        }

        co_return detail::parse_aliyun_lines(*parsed);
    }

    template <typename runtime_type>
    ruvia::task<std::vector<aliyun_record>> list_all_records(runtime_type& c, service::attempt_trace::recorder& trace,
        std::string_view access_key_id, std::string_view access_key_secret, std::string_view domain_name) const {
        std::vector<aliyun_record> result;
        std::int64_t page = 1;
        std::int64_t total_pages = 1;
        do {
            std::map<std::string, std::string> params{
                {"DomainName", std::string(domain_name)},
                {"PageNumber", std::to_string(page)},
                {"PageSize", "100"},
            };
            const auto response = co_await send(c, trace,
                service::attempt_trace::outbound_operation::aliyun_describe_domain_records, "DescribeDomainRecords",
                params, access_key_id, access_key_secret);
            const std::optional<aliyun_record_list_envelope> parsed =
                ruvia::from_json<aliyun_record_list_envelope>(response.body(),
                    {.resource_ = c.pool()});
            if (!response.status().is_successful() || !parsed) {
                fail(c, trace, response.body(), aliyun_error_code::dns_failed, "阿里云 DNS 记录查询失败");
            }
            detail::append_aliyun_records(*parsed, result);
            total_pages = detail::aliyun_page_count(*parsed, result.size());
            ++page;
        } while (page <= total_pages);
        co_return result;
    }

    template <typename runtime_type>
    ruvia::task<std::string>
    create_record(runtime_type& c, service::attempt_trace::recorder& trace, std::string_view access_key_id,
        std::string_view access_key_secret,
        std::string_view domain_name, std::string_view type, std::string_view rr,
        std::string_view value, std::int64_t ttl, std::optional<std::int64_t> priority,
        std::string_view line) const {
        auto params = record_params(domain_name, type, rr, value, ttl, priority, line);
        const auto response = co_await send(c, trace, service::attempt_trace::outbound_operation::aliyun_add_domain_record,
            "AddDomainRecord", params, access_key_id, access_key_secret, std::string(rr) + " " + std::string(type));
        const std::optional<aliyun_record_id_envelope> parsed =
            ruvia::from_json<aliyun_record_id_envelope>(response.body(), {.resource_ = c.pool()});
        if (!response.status().is_successful() || !parsed) {
            fail(c, trace, response.body(), aliyun_error_code::dns_failed, "阿里云 DNS 记录创建失败");
        }
        const auto& record_id_value = parsed->get<"record_id">();
        if (!record_id_value) {
            fail(c, trace, response.body(), aliyun_error_code::dns_failed, "阿里云 DNS 记录创建失败");
        }
        co_return std::string(record_id_value->view());
    }

    template <typename runtime_type>
    ruvia::task<std::string>
    update_record(runtime_type& c, service::attempt_trace::recorder& trace, std::string_view access_key_id,
        std::string_view access_key_secret,
        std::string_view record_id, std::string_view domain_name, std::string_view type,
        std::string_view rr, std::string_view value, std::int64_t ttl,
        std::optional<std::int64_t> priority, std::string_view line) const {
        auto params = record_params(domain_name, type, rr, value, ttl, priority, line);
        params.emplace("RecordId", std::string(record_id));
        const auto response = co_await send(c, trace,
            service::attempt_trace::outbound_operation::aliyun_update_domain_record, "UpdateDomainRecord", params,
            access_key_id, access_key_secret, std::string(rr) + " " + std::string(type));
        const std::optional<aliyun_record_id_envelope> parsed =
            ruvia::from_json<aliyun_record_id_envelope>(response.body(), {.resource_ = c.pool()});
        if (!response.status().is_successful() || !parsed) {
            fail(c, trace, response.body(), aliyun_error_code::dns_failed, "阿里云 DNS 记录更新失败");
        }
        const auto& record_id_value = parsed->get<"record_id">();
        if (!record_id_value) {
            fail(c, trace, response.body(), aliyun_error_code::dns_failed, "阿里云 DNS 记录更新失败");
        }
        co_return std::string(record_id_value->view());
    }

    template <typename runtime_type>
    ruvia::task<void> delete_record(runtime_type& c, service::attempt_trace::recorder& trace,
        std::string_view access_key_id, std::string_view access_key_secret, std::string_view record_id,
        std::string_view subject) const {
        std::map<std::string, std::string> params{{"RecordId", std::string(record_id)}};
        try {
            const auto response = co_await send(c, trace,
                service::attempt_trace::outbound_operation::aliyun_delete_domain_record, "DeleteDomainRecord", params,
                access_key_id, access_key_secret, subject);
            if (!response.status().is_successful()) {
                fail(c, trace, response.body(), aliyun_error_code::dns_failed, "阿里云 DNS 记录删除失败");
            }
        } catch (const aliyun_error& error) {
            const auto message = std::string_view(error.what());
            if (error.code() != aliyun_error_code::record_not_found &&
                message.find("does not exist") == std::string_view::npos) {
                throw;
            }
            trace.log(service::attempt_trace::log_level::warning, "服务商记录已不存在，视为已删除");
        }
        co_return;
    }

private:
    template <typename runtime_type>
    ruvia::task<service::outbound_http::buffered_response>
    send(runtime_type& c, service::attempt_trace::recorder& trace, service::attempt_trace::outbound_operation operation,
        std::string_view action, const std::map<std::string, std::string>& params, std::string_view access_key_id,
        std::string_view access_key_secret, std::string_view subject = {}) const {
        // The credentials of every request are the attempt's: whatever the
        // reply or an error echoes of them is redacted.
        trace.register_secret(access_key_id);
        trace.register_secret(access_key_secret);
        auto&& client = c.get_http_client(service::config::aliyun_dns_origin_alias);
        const auto target = "/?" + signed_query(action, params, access_key_id, access_key_secret);
        std::array<ruvia::http_header_view, 1> headers{
            ruvia::http_header_view{"accept", "application/json"},
        };
        try {
            co_return co_await service::outbound_http::send_buffered(
                client, {.origin_alias_ = service::config::aliyun_dns_origin_alias, .operation_ = operation, .subject_ = subject},
                trace,
                {
                    .method_ = ruvia::known_http_method_token(ruvia::http_known_method::get),
                    .target_ = target,
                    .headers_ = headers,
                    .content_ = ruvia::http_client_request_content_view::none(),
                },
                {.timeout_ = std::chrono::seconds(10), .stop_token_ = c.get_stop_token()});
        } catch (const ruvia::http_client_error& error) {
            using code = ruvia::http_client_error::code_type;
            switch (error.code()) {
                case code::timeout:
                    throw aliyun_error(aliyun_error_code::upstream_failed,
                        "阿里云 DNS API 请求超时（10 秒）");
                case code::resolve_failed:
                    throw aliyun_error(aliyun_error_code::upstream_failed, "阿里云 DNS API 域名解析失败");
                case code::connect_failed:
                    throw aliyun_error(aliyun_error_code::upstream_failed, "阿里云 DNS API 连接失败");
                case code::tls_failed:
                    throw aliyun_error(aliyun_error_code::upstream_failed, "阿里云 DNS API TLS 连接失败");
                case code::response_too_large:
                    throw aliyun_error(aliyun_error_code::upstream_failed,
                        "阿里云 DNS API 响应超过大小限制");
                case code::io_error:
                    throw aliyun_error(aliyun_error_code::upstream_failed, "阿里云 DNS API 网络读写失败");
                case code::protocol_unavailable:
                case code::protocol_error:
                    throw aliyun_error(aliyun_error_code::upstream_failed, "阿里云 DNS API 响应协议错误");
                case code::queue_full:
                    throw aliyun_error(aliyun_error_code::upstream_failed, "阿里云 DNS API 请求队列繁忙");
                case code::cancelled:
                case code::closing:
                    throw aliyun_error(aliyun_error_code::upstream_failed, "阿里云 DNS API 请求已取消");
                default:
                    throw aliyun_error(aliyun_error_code::upstream_failed, "阿里云 DNS API 请求失败");
            }
        }
    }

    [[nodiscard]] static std::map<std::string, std::string>
    record_params(std::string_view domain_name, std::string_view type, std::string_view rr,
        std::string_view value, std::int64_t ttl, std::optional<std::int64_t> priority,
        std::string_view line) {
        std::map<std::string, std::string> params{
            {"DomainName", std::string(domain_name)},
            {"RR", std::string(rr)},
            {"Type", std::string(type)},
            {"Value", std::string(value)},
            {"TTL", std::to_string(ttl)},
            {"Line", line.empty() ? "default" : std::string(line)},
        };
        if (type == "MX" && priority) {
            params.emplace("Priority", std::to_string(*priority));
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
        const auto string_to_sign = "GET&%2F&" + percent_encode(canonical);
        all.emplace("Signature", hmac_sha1_base64(access_key_secret, string_to_sign));
        return canonical_query(all);
    }

    [[nodiscard]] static std::string canonical_query(const std::map<std::string, std::string>& all) {
        std::string result;
        for (const auto& [key, value] : all) {
            if (!result.empty()) {
                result.push_back('&');
            }
            result += percent_encode(key);
            result.push_back('=');
            result += percent_encode(value);
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
            throw aliyun_error(aliyun_error_code::upstream_failed, "阿里云 DNS 签名随机数生成失败");
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
            throw aliyun_error(aliyun_error_code::upstream_failed, "阿里云 DNS 请求签名失败");
        }
        std::string output(4 * ((digest_size + 2) / 3), '\0');
        const auto size = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(output.data()),
            digest.data(), static_cast<int>(digest_size));
        if (size < 0) {
            throw aliyun_error(aliyun_error_code::upstream_failed, "阿里云 DNS 请求签名编码失败");
        }
        output.resize(static_cast<std::size_t>(size));
        return output;
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

    template <typename runtime_type>
    [[noreturn]] static void fail(runtime_type& c, service::attempt_trace::recorder& trace, std::string_view body,
        aliyun_error_code fallback_code, std::string_view fallback_message) {
        throw detail::aliyun_reply_error(trace, body, fallback_code, fallback_message, c.pool());
    }
};

inline const aliyun_client& get_aliyun_client() {
    static const aliyun_client client;
    return client;
}

}  // namespace service::dns
