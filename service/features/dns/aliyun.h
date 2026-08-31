#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <map>
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

#include <ruvia/core/Task.h>
#include <ruvia/http/HttpKnownMethod.h>
#include <ruvia/web/Context.h>
#include <ruvia/web/HttpClientHandle.h>
#include <ruvia/web/Model.h>

#include "service/config/outbound.h"
#include "service/features/outbound_http/client.h"
#include "service/utils/sensitive_string.h"

namespace service::dns {

RUVIA_REQUEST_MODEL(AliyunErrorEnvelope, RUVIA_OPTIONAL_FIELD_NAME("Code", code, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("Message", message, ruvia::String));

RUVIA_REQUEST_MODEL(AliyunDomainPayload,
                    RUVIA_OPTIONAL_FIELD_NAME("DomainId", domainId, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("DomainName", domainName, ruvia::String));
RUVIA_REQUEST_MODEL(AliyunDomainListPayload,
                    RUVIA_OPTIONAL_FIELD_NAME("Domain", domain, ruvia::Array<AliyunDomainPayload>));
RUVIA_REQUEST_MODEL(AliyunDomainListEnvelope,
                    RUVIA_OPTIONAL_FIELD_NAME("Domains", domains, AliyunDomainListPayload),
                    RUVIA_OPTIONAL_FIELD_NAME("TotalCount", totalCount, ruvia::Int64));

RUVIA_REQUEST_MODEL(AliyunLinePayload,
                    RUVIA_OPTIONAL_FIELD_NAME("LineCode", lineCode, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("LineName", lineName, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("LineDisplayName", lineDisplayName, ruvia::String));
RUVIA_REQUEST_MODEL(AliyunLineListPayload,
                    RUVIA_OPTIONAL_FIELD_NAME("RecordLine", recordLine,
                                              ruvia::Array<AliyunLinePayload>));
RUVIA_REQUEST_MODEL(AliyunLineListEnvelope,
                    RUVIA_OPTIONAL_FIELD_NAME("RecordLines", recordLines, AliyunLineListPayload));

RUVIA_REQUEST_MODEL(AliyunRecordPayload,
                    RUVIA_OPTIONAL_FIELD_NAME("RecordId", recordId, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("RR", rr, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("Type", type, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("Value", value, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("TTL", ttl, ruvia::Int64),
                    RUVIA_OPTIONAL_FIELD_NAME("Priority", priority, ruvia::Int64),
                    RUVIA_OPTIONAL_FIELD_NAME("Line", line, ruvia::String));
RUVIA_REQUEST_MODEL(AliyunRecordListPayload,
                    RUVIA_OPTIONAL_FIELD_NAME("Record", record, ruvia::Array<AliyunRecordPayload>));
RUVIA_REQUEST_MODEL(AliyunRecordListEnvelope,
                    RUVIA_OPTIONAL_FIELD_NAME("DomainRecords", domainRecords,
                                              AliyunRecordListPayload),
                    RUVIA_OPTIONAL_FIELD_NAME("TotalCount", totalCount, ruvia::Int64));
RUVIA_REQUEST_MODEL(AliyunRecordIdEnvelope,
                    RUVIA_OPTIONAL_FIELD_NAME("RecordId", recordId, ruvia::String));

enum class AliyunErrorCode {
    credentialInvalid,
    authorizationFailed,
    upstreamFailed,
    domainNotFound,
    dnsFailed,
    recordConflict,
    recordNotFound,
};

class AliyunError final : public std::runtime_error {
  public:
    AliyunError(AliyunErrorCode code, std::string_view message)
        : std::runtime_error(std::string(message)), code_(code) {}

    [[nodiscard]] AliyunErrorCode code() const noexcept { return code_; }

  private:
    AliyunErrorCode code_;
};

struct AliyunDomain {
    std::string id;
    std::string name;
    std::string status;
};

struct AliyunLine {
    std::string code;
    std::string name;
    std::string displayName;
};

struct AliyunRecord {
    std::string id;
    std::string rr;
    std::string type;
    std::string value;
    std::int64_t ttl;
    std::optional<std::int64_t> priority;
    std::string line;
};

namespace detail {

inline void appendAliyunDomains(const AliyunDomainListEnvelope& envelope,
                                std::vector<AliyunDomain>& result) {
    const auto& domains = envelope.get<"domains">();
    if (!domains) {
        return;
    }
    const auto& domainList = domains->get<"domain">();
    if (!domainList) {
        return;
    }
    for (const auto& domain : *domainList) {
        const auto& name = domain.get<"domainName">();
        if (!name || name->view().empty()) {
            continue;
        }
        const auto& id = domain.get<"domainId">();
        result.push_back(AliyunDomain{
            id && !id->view().empty() ? std::string(id->view()) : std::string(name->view()),
            std::string(name->view()),
            "active",
        });
    }
}

inline std::vector<AliyunLine> parseAliyunLines(const AliyunLineListEnvelope& envelope) {
    std::vector<AliyunLine> result;
    const auto& lines = envelope.get<"recordLines">();
    if (lines) {
        const auto& lineList = lines->get<"recordLine">();
        if (lineList) {
            for (const auto& line : *lineList) {
                const auto& code = line.get<"lineCode">();
                if (!code || code->view().empty()) {
                    continue;
                }
                const auto& name = line.get<"lineName">();
                const auto& display = line.get<"lineDisplayName">();
                auto resolvedName = name && !name->view().empty() ? std::string(name->view())
                                                                  : std::string(code->view());
                auto resolvedDisplay = display && !display->view().empty()
                                           ? std::string(display->view())
                                           : resolvedName;
                result.push_back(AliyunLine{std::string(code->view()), std::move(resolvedName),
                                            std::move(resolvedDisplay)});
            }
        }
    }
    if (std::none_of(result.begin(), result.end(),
                     [](const auto& line) { return line.code == "default"; })) {
        result.insert(result.begin(), AliyunLine{"default", "默认", "默认"});
    }
    return result;
}

inline void appendAliyunRecords(const AliyunRecordListEnvelope& envelope,
                                std::vector<AliyunRecord>& result) {
    const auto& records = envelope.get<"domainRecords">();
    if (!records) {
        return;
    }
    const auto& recordList = records->get<"record">();
    if (!recordList) {
        return;
    }
    for (const auto& record : *recordList) {
        const auto& id = record.get<"recordId">();
        const auto& rr = record.get<"rr">();
        const auto& type = record.get<"type">();
        const auto& value = record.get<"value">();
        const auto& ttl = record.get<"ttl">();
        if (!id || !rr || !type || !value || !ttl) {
            continue;
        }
        AliyunRecord item{std::string(id->view()),
                          std::string(rr->view()),
                          std::string(type->view()),
                          std::string(value->view()),
                          static_cast<std::int64_t>(*ttl),
                          std::nullopt,
                          "default"};
        const auto& priority = record.get<"priority">();
        if (priority) {
            item.priority = static_cast<std::int64_t>(*priority);
        }
        const auto& line = record.get<"line">();
        if (line && !line->view().empty()) {
            item.line = std::string(line->view());
        }
        result.push_back(std::move(item));
    }
}

template <typename Envelope>
inline std::int64_t aliyunPageCount(const Envelope& envelope, std::size_t fallback) {
    const auto& totalCount = envelope.template get<"totalCount">();
    const auto total =
        totalCount ? static_cast<std::int64_t>(*totalCount) : static_cast<std::int64_t>(fallback);
    return std::max<std::int64_t>(1, (total + 99) / 100);
}

} // namespace detail

class AliyunClient final {
  public:
    template <typename Runtime>
    ruvia::Task<std::vector<AliyunDomain>> verifyAccessKey(Runtime& c, std::string_view accessKeyId,
                                                           std::string_view accessKeySecret) const {
        auto domains = co_await listDomains(c, accessKeyId, accessKeySecret);
        (void)co_await listSupportLines(c, accessKeyId, accessKeySecret, std::nullopt);
        co_return domains;
    }

    template <typename Runtime>
    ruvia::Task<std::vector<AliyunDomain>> listDomains(Runtime& c, std::string_view accessKeyId,
                                                       std::string_view accessKeySecret) const {
        std::vector<AliyunDomain> result;
        std::int64_t page = 1;
        std::int64_t totalPages = 1;
        do {
            std::map<std::string, std::string> params{
                {"PageNumber", std::to_string(page)},
                {"PageSize", "100"},
            };
            const auto response =
                co_await send(c, "DescribeDomains", params, accessKeyId, accessKeySecret);
            const std::optional<AliyunDomainListEnvelope> parsed =
                ruvia::fromJson<AliyunDomainListEnvelope>(response.body(),
                                                          {.resource = c.resource()});
            if (!response.status().isSuccessful() || !parsed) {
                fail(c, response.body(), AliyunErrorCode::upstreamFailed,
                     "阿里云 DNS 域名查询失败");
            }
            detail::appendAliyunDomains(*parsed, result);
            totalPages = detail::aliyunPageCount(*parsed, result.size());
            ++page;
        } while (page <= totalPages);
        co_return result;
    }

    template <typename Runtime>
    ruvia::Task<std::vector<AliyunLine>>
    listSupportLines(Runtime& c, std::string_view accessKeyId, std::string_view accessKeySecret,
                     std::optional<std::string_view> domainName) const {
        std::map<std::string, std::string> params;
        if (domainName && !domainName->empty()) {
            params.emplace("DomainName", std::string(*domainName));
        }
        const auto response =
            co_await send(c, "DescribeSupportLines", params, accessKeyId, accessKeySecret);
        const std::optional<AliyunLineListEnvelope> parsed =
            ruvia::fromJson<AliyunLineListEnvelope>(response.body(), {.resource = c.resource()});
        if (!response.status().isSuccessful() || !parsed) {
            fail(c, response.body(), AliyunErrorCode::upstreamFailed, "阿里云 DNS 线路查询失败");
        }

        co_return detail::parseAliyunLines(*parsed);
    }

    template <typename Runtime>
    ruvia::Task<std::vector<AliyunRecord>> listAllRecords(Runtime& c, std::string_view accessKeyId,
                                                          std::string_view accessKeySecret,
                                                          std::string_view domainName) const {
        std::vector<AliyunRecord> result;
        std::int64_t page = 1;
        std::int64_t totalPages = 1;
        do {
            std::map<std::string, std::string> params{
                {"DomainName", std::string(domainName)},
                {"PageNumber", std::to_string(page)},
                {"PageSize", "100"},
            };
            const auto response =
                co_await send(c, "DescribeDomainRecords", params, accessKeyId, accessKeySecret);
            const std::optional<AliyunRecordListEnvelope> parsed =
                ruvia::fromJson<AliyunRecordListEnvelope>(response.body(),
                                                          {.resource = c.resource()});
            if (!response.status().isSuccessful() || !parsed) {
                fail(c, response.body(), AliyunErrorCode::dnsFailed, "阿里云 DNS 记录查询失败");
            }
            detail::appendAliyunRecords(*parsed, result);
            totalPages = detail::aliyunPageCount(*parsed, result.size());
            ++page;
        } while (page <= totalPages);
        co_return result;
    }

    template <typename Runtime>
    ruvia::Task<std::string>
    reconcileRecord(Runtime& c, std::string_view accessKeyId, std::string_view accessKeySecret,
                    std::string_view domainName, std::string_view remoteRecordId,
                    std::string_view type, std::string_view rr, std::string_view value,
                    std::int64_t ttl, std::optional<std::int64_t> priority, std::string_view line,
                    const std::vector<AliyunRecord>& remoteRecords) const {
        if (!remoteRecordId.empty()) {
            const auto byId = std::find_if(
                remoteRecords.begin(), remoteRecords.end(),
                [remoteRecordId](const auto& record) { return record.id == remoteRecordId; });
            if (byId != remoteRecords.end()) {
                if (recordMatches(*byId, type, rr, value, ttl, priority, line)) {
                    co_return byId->id;
                }
                co_return co_await updateRecord(c, accessKeyId, accessKeySecret, byId->id,
                                                domainName, type, rr, value, ttl, priority, line);
            }
        }

        const AliyunRecord* exact = nullptr;
        for (const auto& record : remoteRecords) {
            if (record.type != type || !recordNameEquals(record.rr, rr) || record.value != value ||
                record.line != line) {
                continue;
            }
            if (exact && exact->id != record.id) {
                throw AliyunError(AliyunErrorCode::recordConflict,
                                  "阿里云 DNS 中存在多条相同线路解析记录，无法安全接管");
            }
            exact = &record;
        }
        if (exact) {
            if (recordMatches(*exact, type, rr, value, ttl, priority, line)) {
                co_return exact->id;
            }
            co_return co_await updateRecord(c, accessKeyId, accessKeySecret, exact->id, domainName,
                                            type, rr, value, ttl, priority, line);
        }

        co_return co_await createRecord(c, accessKeyId, accessKeySecret, domainName, type, rr,
                                        value, ttl, priority, line);
    }

    template <typename Runtime>
    ruvia::Task<std::string>
    createRecord(Runtime& c, std::string_view accessKeyId, std::string_view accessKeySecret,
                 std::string_view domainName, std::string_view type, std::string_view rr,
                 std::string_view value, std::int64_t ttl, std::optional<std::int64_t> priority,
                 std::string_view line) const {
        auto params = recordParams(domainName, type, rr, value, ttl, priority, line);
        const auto response =
            co_await send(c, "AddDomainRecord", params, accessKeyId, accessKeySecret);
        const std::optional<AliyunRecordIdEnvelope> parsed =
            ruvia::fromJson<AliyunRecordIdEnvelope>(response.body(), {.resource = c.resource()});
        if (!response.status().isSuccessful() || !parsed) {
            fail(c, response.body(), AliyunErrorCode::dnsFailed, "阿里云 DNS 记录创建失败");
        }
        const auto& recordIdValue = parsed->get<"recordId">();
        if (!recordIdValue) {
            fail(c, response.body(), AliyunErrorCode::dnsFailed, "阿里云 DNS 记录创建失败");
        }
        co_return std::string(recordIdValue->view());
    }

    template <typename Runtime>
    ruvia::Task<std::string>
    updateRecord(Runtime& c, std::string_view accessKeyId, std::string_view accessKeySecret,
                 std::string_view recordId, std::string_view domainName, std::string_view type,
                 std::string_view rr, std::string_view value, std::int64_t ttl,
                 std::optional<std::int64_t> priority, std::string_view line) const {
        auto params = recordParams(domainName, type, rr, value, ttl, priority, line);
        params.emplace("RecordId", std::string(recordId));
        const auto response =
            co_await send(c, "UpdateDomainRecord", params, accessKeyId, accessKeySecret);
        const std::optional<AliyunRecordIdEnvelope> parsed =
            ruvia::fromJson<AliyunRecordIdEnvelope>(response.body(), {.resource = c.resource()});
        if (!response.status().isSuccessful() || !parsed) {
            fail(c, response.body(), AliyunErrorCode::dnsFailed, "阿里云 DNS 记录更新失败");
        }
        const auto& recordIdValue = parsed->get<"recordId">();
        if (!recordIdValue) {
            fail(c, response.body(), AliyunErrorCode::dnsFailed, "阿里云 DNS 记录更新失败");
        }
        co_return std::string(recordIdValue->view());
    }

    template <typename Runtime>
    ruvia::Task<void> deleteRecord(Runtime& c, std::string_view accessKeyId,
                                   std::string_view accessKeySecret,
                                   std::string_view recordId) const {
        std::map<std::string, std::string> params{{"RecordId", std::string(recordId)}};
        try {
            const auto response =
                co_await send(c, "DeleteDomainRecord", params, accessKeyId, accessKeySecret);
            if (!response.status().isSuccessful()) {
                fail(c, response.body(), AliyunErrorCode::dnsFailed, "阿里云 DNS 记录删除失败");
            }
        } catch (const AliyunError& error) {
            const auto message = std::string_view(error.what());
            if (error.code() != AliyunErrorCode::recordNotFound &&
                message.find("does not exist") == std::string_view::npos) {
                throw;
            }
        }
        co_return;
    }

  private:
    template <typename Runtime>
    ruvia::Task<service::outbound_http::BufferedResponse>
    send(Runtime& c, std::string_view action, const std::map<std::string, std::string>& params,
         std::string_view accessKeyId, std::string_view accessKeySecret) const {
        auto&& client = c.httpClient(service::config::kAliyunDnsOriginAlias);
        const auto target = "/?" + signedQuery(action, params, accessKeyId, accessKeySecret);
        std::array<ruvia::HttpHeaderView, 1> headers{
            ruvia::HttpHeaderView{"accept", "application/json"},
        };
        try {
            co_return co_await service::outbound_http::sendBuffered(
                client,
                {
                    .method = ruvia::knownHttpMethodToken(ruvia::HttpKnownMethod::kGet),
                    .target = target,
                    .headers = headers,
                    .content = ruvia::HttpClientRequestContentView::none(),
                },
                {.timeout = std::chrono::seconds(10), .stopToken = c.stopToken()});
        } catch (const ruvia::HttpClientError& error) {
            using Code = ruvia::HttpClientError::Code;
            switch (error.code()) {
            case Code::kTimeout:
                throw AliyunError(AliyunErrorCode::upstreamFailed,
                                  "阿里云 DNS API 请求超时（10 秒）");
            case Code::kResolveFailed:
                throw AliyunError(AliyunErrorCode::upstreamFailed, "阿里云 DNS API 域名解析失败");
            case Code::kConnectFailed:
                throw AliyunError(AliyunErrorCode::upstreamFailed, "阿里云 DNS API 连接失败");
            case Code::kTlsFailed:
                throw AliyunError(AliyunErrorCode::upstreamFailed, "阿里云 DNS API TLS 连接失败");
            case Code::kResponseTooLarge:
                throw AliyunError(AliyunErrorCode::upstreamFailed,
                                  "阿里云 DNS API 响应超过大小限制");
            case Code::kIoError:
                throw AliyunError(AliyunErrorCode::upstreamFailed, "阿里云 DNS API 网络读写失败");
            case Code::kProtocolUnavailable:
            case Code::kProtocolError:
                throw AliyunError(AliyunErrorCode::upstreamFailed, "阿里云 DNS API 响应协议错误");
            case Code::kQueueFull:
                throw AliyunError(AliyunErrorCode::upstreamFailed, "阿里云 DNS API 请求队列繁忙");
            case Code::kCancelled:
            case Code::kClosing:
                throw AliyunError(AliyunErrorCode::upstreamFailed, "阿里云 DNS API 请求已取消");
            default:
                throw AliyunError(AliyunErrorCode::upstreamFailed, "阿里云 DNS API 请求失败");
            }
        }
    }

    [[nodiscard]] static std::map<std::string, std::string>
    recordParams(std::string_view domainName, std::string_view type, std::string_view rr,
                 std::string_view value, std::int64_t ttl, std::optional<std::int64_t> priority,
                 std::string_view line) {
        std::map<std::string, std::string> params{
            {"DomainName", std::string(domainName)},
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

    [[nodiscard]] static bool recordMatches(const AliyunRecord& remote, std::string_view type,
                                            std::string_view rr, std::string_view value,
                                            std::int64_t ttl, std::optional<std::int64_t> priority,
                                            std::string_view line) {
        if (remote.type != type || !recordNameEquals(remote.rr, rr) || remote.value != value ||
            remote.ttl != ttl || remote.line != line) {
            return false;
        }
        if (type == "MX" && remote.priority != priority) {
            return false;
        }
        return true;
    }

    [[nodiscard]] static bool recordNameEquals(std::string_view left, std::string_view right) {
        return normalize(left) == normalize(right);
    }

    [[nodiscard]] static std::string normalize(std::string_view input) {
        std::string result(input);
        std::transform(result.begin(), result.end(), result.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return result;
    }

    [[nodiscard]] static std::string signedQuery(std::string_view action,
                                                 const std::map<std::string, std::string>& params,
                                                 std::string_view accessKeyId,
                                                 std::string_view accessKeySecret) {
        std::map<std::string, std::string> all = params;
        all.emplace("Action", std::string(action));
        all.emplace("AccessKeyId", std::string(accessKeyId));
        all.emplace("Format", "JSON");
        all.emplace("SignatureMethod", "HMAC-SHA1");
        all.emplace("SignatureNonce", nonce());
        all.emplace("SignatureVersion", "1.0");
        all.emplace("Timestamp", timestamp());
        all.emplace("Version", "2015-01-09");

        const auto canonical = canonicalQuery(all);
        const auto stringToSign = "GET&%2F&" + percentEncode(canonical);
        all.emplace("Signature", hmacSha1Base64(accessKeySecret, stringToSign));
        return canonicalQuery(all);
    }

    [[nodiscard]] static std::string canonicalQuery(const std::map<std::string, std::string>& all) {
        std::string result;
        for (const auto& [key, value] : all) {
            if (!result.empty()) {
                result.push_back('&');
            }
            result += percentEncode(key);
            result.push_back('=');
            result += percentEncode(value);
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
        const service::utils::SensitiveBufferGuard cleanseBytes(bytes);
        if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
            throw AliyunError(AliyunErrorCode::upstreamFailed, "阿里云 DNS 签名随机数生成失败");
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

    [[nodiscard]] static std::string hmacSha1Base64(std::string_view secret,
                                                    std::string_view payload) {
        service::utils::SensitiveString key(std::string(secret) + "&");
        std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
        const service::utils::SensitiveBufferGuard cleanseDigest(digest);
        unsigned int digestSize = 0;
        if (HMAC(EVP_sha1(), key.view().data(), static_cast<int>(key.view().size()),
                 reinterpret_cast<const unsigned char*>(payload.data()), payload.size(),
                 digest.data(), &digestSize) == nullptr) {
            throw AliyunError(AliyunErrorCode::upstreamFailed, "阿里云 DNS 请求签名失败");
        }
        std::string output(4 * ((digestSize + 2) / 3), '\0');
        const auto size = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(output.data()),
                                          digest.data(), static_cast<int>(digestSize));
        if (size < 0) {
            throw AliyunError(AliyunErrorCode::upstreamFailed, "阿里云 DNS 请求签名编码失败");
        }
        output.resize(static_cast<std::size_t>(size));
        return output;
    }

    [[nodiscard]] static std::string percentEncode(std::string_view input) {
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

    template <typename Runtime>
    [[noreturn]] static void fail(Runtime& c, std::string_view body, AliyunErrorCode fallbackCode,
                                  std::string_view fallbackMessage) {
        auto code = fallbackCode;
        auto message = std::string(fallbackMessage);
        const std::optional<AliyunErrorEnvelope> parsed =
            ruvia::fromJson<AliyunErrorEnvelope>(body, {.resource = c.resource()});
        if (parsed) {
            const auto& remoteCode = parsed->get<"code">();
            const auto& remoteMessage = parsed->get<"message">();
            if (remoteMessage && !remoteMessage->view().empty()) {
                message = std::string(remoteMessage->view());
            }
            if (remoteCode) {
                const auto value = std::string(remoteCode->view());
                if (value.find("InvalidAccessKeyId") != std::string::npos ||
                    value.find("Signature") != std::string::npos) {
                    code = AliyunErrorCode::credentialInvalid;
                } else if (value.find("Forbidden") != std::string::npos ||
                           value.find("Unauthorized") != std::string::npos) {
                    code = AliyunErrorCode::authorizationFailed;
                } else if (value.find("DomainRecord") != std::string::npos &&
                           value.find("NotFound") != std::string::npos) {
                    code = AliyunErrorCode::recordNotFound;
                } else if (value.find("Domain") != std::string::npos &&
                           value.find("NotFound") != std::string::npos) {
                    code = AliyunErrorCode::domainNotFound;
                } else if (value.find("Duplicate") != std::string::npos ||
                           value.find("RecordForbidden") != std::string::npos) {
                    code = AliyunErrorCode::recordConflict;
                }
            }
        }
        throw AliyunError(code, std::move(message));
    }
};

inline const AliyunClient& aliyunClient() {
    static const AliyunClient client;
    return client;
}

} // namespace service::dns
