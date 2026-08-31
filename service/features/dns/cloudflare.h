#pragma once

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

#include <ruvia/core/Task.h>
#include <ruvia/http/HttpKnownMethod.h>
#include <ruvia/web/Context.h>
#include <ruvia/web/HttpClientHandle.h>
#include <ruvia/web/Model.h>

#include "service/config/outbound.h"
#include "service/features/outbound_http/client.h"
#include "service/utils/sensitive_string.h"

namespace service::dns {

RUVIA_REQUEST_MODEL(CloudflareApiErrorPayload, RUVIA_OPTIONAL_FIELD(code, ruvia::Int64),
                    RUVIA_OPTIONAL_FIELD(message, ruvia::String));

RUVIA_REQUEST_MODEL(CloudflareErrorEnvelope, RUVIA_OPTIONAL_FIELD(success, ruvia::Bool),
                    RUVIA_OPTIONAL_FIELD(errors, ruvia::Array<CloudflareApiErrorPayload>));

RUVIA_REQUEST_MODEL(CloudflareTokenPayload, RUVIA_OPTIONAL_FIELD(id, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(status, ruvia::String));

RUVIA_REQUEST_MODEL(CloudflareTokenEnvelope, RUVIA_OPTIONAL_FIELD(success, ruvia::Bool),
                    RUVIA_OPTIONAL_FIELD(result, CloudflareTokenPayload),
                    RUVIA_OPTIONAL_FIELD(errors, ruvia::Array<CloudflareApiErrorPayload>));

RUVIA_REQUEST_MODEL(CloudflareZonePayload, RUVIA_OPTIONAL_FIELD(id, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(name, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(status, ruvia::String));

RUVIA_REQUEST_MODEL(CloudflareResultInfo, RUVIA_OPTIONAL_FIELD(page, ruvia::Int64),
                    RUVIA_OPTIONAL_FIELD_NAME("per_page", perPage, ruvia::Int64),
                    RUVIA_OPTIONAL_FIELD_NAME("total_count", totalCount, ruvia::Int64),
                    RUVIA_OPTIONAL_FIELD_NAME("total_pages", totalPages, ruvia::Int64));

RUVIA_REQUEST_MODEL(CloudflareZoneEnvelope, RUVIA_OPTIONAL_FIELD(success, ruvia::Bool),
                    RUVIA_OPTIONAL_FIELD(result, ruvia::Array<CloudflareZonePayload>),
                    RUVIA_OPTIONAL_FIELD_NAME("result_info", resultInfo, CloudflareResultInfo),
                    RUVIA_OPTIONAL_FIELD(errors, ruvia::Array<CloudflareApiErrorPayload>));

RUVIA_REQUEST_MODEL(CloudflareRecordPayload, RUVIA_OPTIONAL_FIELD(id, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(type, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(name, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(content, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(ttl, ruvia::Int64),
                    RUVIA_OPTIONAL_FIELD(priority, ruvia::Int64),
                    RUVIA_OPTIONAL_FIELD(proxied, ruvia::Bool),
                    RUVIA_OPTIONAL_FIELD_NAME("modified_on", modifiedOn, ruvia::String));

RUVIA_REQUEST_MODEL(CloudflareRecordEnvelope, RUVIA_OPTIONAL_FIELD(success, ruvia::Bool),
                    RUVIA_OPTIONAL_FIELD(result, CloudflareRecordPayload),
                    RUVIA_OPTIONAL_FIELD(errors, ruvia::Array<CloudflareApiErrorPayload>));

RUVIA_REQUEST_MODEL(CloudflareRecordListEnvelope, RUVIA_OPTIONAL_FIELD(success, ruvia::Bool),
                    RUVIA_OPTIONAL_FIELD(result, ruvia::Array<CloudflareRecordPayload>),
                    RUVIA_OPTIONAL_FIELD_NAME("result_info", resultInfo, CloudflareResultInfo),
                    RUVIA_OPTIONAL_FIELD(errors, ruvia::Array<CloudflareApiErrorPayload>));

RUVIA_REQUEST_MODEL(CloudflareDeleteEnvelope, RUVIA_OPTIONAL_FIELD(success, ruvia::Bool),
                    RUVIA_OPTIONAL_FIELD(errors, ruvia::Array<CloudflareApiErrorPayload>));

RUVIA_RESPONSE_MODEL(CloudflareSaveRecordPayload, RUVIA_REQUIRED_FIELD(type, ruvia::String),
                     RUVIA_REQUIRED_FIELD(name, ruvia::String),
                     RUVIA_REQUIRED_FIELD(content, ruvia::String),
                     RUVIA_REQUIRED_FIELD(ttl, ruvia::Int64),
                     RUVIA_OPTIONAL_FIELD(priority, ruvia::Int64, RUVIA_OMIT_EMPTY),
                     RUVIA_OPTIONAL_FIELD(proxied, ruvia::Bool, RUVIA_OMIT_EMPTY));

enum class CloudflareErrorCode {
    credentialInvalid,
    authorizationFailed,
    upstreamFailed,
    zoneNotFound,
    dnsFailed,
    recordConflict,
    recordNotFound,
};

class CloudflareError final : public std::runtime_error {
  public:
    CloudflareError(CloudflareErrorCode code, std::string_view message)
        : std::runtime_error(std::string(message)), code_(code) {}

    [[nodiscard]] CloudflareErrorCode code() const noexcept { return code_; }

  private:
    CloudflareErrorCode code_;
};

struct CloudflareZone {
    std::string id;
    std::string name;
    std::string status;
};

struct CloudflareRecord {
    std::string id;
    std::string type;
    std::string name;
    std::string content;
    std::int64_t ttl;
    std::optional<std::int64_t> priority;
    bool proxied;
    std::string modifiedAt;
};

struct CloudflareRecordPage {
    std::vector<CloudflareRecord> list;
    std::int64_t total;
    std::int64_t totalPages;
};

class CloudflareClient final {
  public:
    template <typename Runtime>
    ruvia::Task<std::vector<CloudflareZone>> verifyToken(Runtime& c, std::string_view token,
                                                         std::string_view accountId) const {
        const auto response = co_await send(
            c, ruvia::HttpKnownMethod::kGet,
            "/client/v4/accounts/" + std::string(accountId) + "/tokens/verify", token);
        const std::optional<CloudflareTokenEnvelope> parsed =
            ruvia::fromJson<CloudflareTokenEnvelope>(response.body(), {.resource = c.resource()});
        if (!response.status().isSuccessful() || !parsed || !success(*parsed)) {
            fail(parsed, CloudflareErrorCode::credentialInvalid,
                 "Cloudflare API Token 无效或未激活");
        }
        const auto& result = parsed->get<"result">();
        if (!result) {
            throw CloudflareError(CloudflareErrorCode::credentialInvalid,
                                  "Cloudflare API Token 无效或未激活");
        }
        const auto& status = result->get<"status">();
        if (!status || status->view() != "active") {
            throw CloudflareError(CloudflareErrorCode::credentialInvalid,
                                  "Cloudflare API Token 无效或未激活");
        }

        co_return co_await listZones(c, token, accountId);
    }

    template <typename Runtime>
    ruvia::Task<std::vector<CloudflareZone>> listZones(Runtime& c, std::string_view token,
                                                       std::string_view accountId) const {
        std::vector<CloudflareZone> result;
        std::int64_t page = 1;
        std::int64_t totalPages = 1;
        do {
            const auto target =
                "/client/v4/zones?account.id=" + percentEncode(accountId) +
                "&status=active&order=name&direction=asc&per_page=50&page=" + std::to_string(page);
            const auto response = co_await send(c, ruvia::HttpKnownMethod::kGet, target, token);
            const std::optional<CloudflareZoneEnvelope> parsed =
                ruvia::fromJson<CloudflareZoneEnvelope>(response.body(),
                                                        {.resource = c.resource()});
            if (!response.status().isSuccessful() || !parsed || !success(*parsed)) {
                fail(parsed, CloudflareErrorCode::credentialInvalid,
                     "Cloudflare API Token 缺少 Zone Read 权限");
            }
            const auto& zones = parsed->get<"result">();
            if (zones) {
                for (const auto& zone : *zones) {
                    const auto& id = zone.get<"id">();
                    const auto& name = zone.get<"name">();
                    const auto& status = zone.get<"status">();
                    if (id && name && status) {
                        result.push_back(CloudflareZone{std::string(id->view()),
                                                        std::string(name->view()),
                                                        std::string(status->view())});
                    }
                }
            }
            totalPages = 1;
            const auto& info = parsed->get<"resultInfo">();
            if (info) {
                const auto& value = info->get<"totalPages">();
                if (value) {
                    totalPages = static_cast<std::int64_t>(*value);
                }
            }
            ++page;
        } while (page <= totalPages);
        co_return result;
    }

    ruvia::Task<CloudflareZone> findZoneById(ruvia::Context& c, std::string_view token,
                                             std::string_view accountId,
                                             std::string_view zoneId) const {
        const auto zones = co_await listZones(c, token, accountId);
        const auto found = std::find_if(zones.begin(), zones.end(),
                                        [zoneId](const auto& zone) { return zone.id == zoneId; });
        if (found == zones.end()) {
            throw CloudflareError(CloudflareErrorCode::zoneNotFound,
                                  "Cloudflare 中未找到该活动 Zone");
        }
        co_return *found;
    }

    template <typename Runtime>
    ruvia::Task<CloudflareZone> findZone(Runtime& c, std::string_view token,
                                         std::string_view accountId,
                                         std::string_view domain) const {
        std::string candidate = normalizeDomain(domain);
        if (candidate.starts_with("*.")) {
            candidate.erase(0, 2);
        }

        while (candidate.find('.') != std::string::npos) {
            const auto target = "/client/v4/zones?account.id=" + percentEncode(accountId) +
                                "&name=" + percentEncode(candidate) + "&status=active&per_page=5";
            const auto response = co_await send(c, ruvia::HttpKnownMethod::kGet, target, token);
            const std::optional<CloudflareZoneEnvelope> parsed =
                ruvia::fromJson<CloudflareZoneEnvelope>(response.body(),
                                                        {.resource = c.resource()});
            if (!response.status().isSuccessful() || !parsed || !success(*parsed)) {
                fail(parsed, CloudflareErrorCode::upstreamFailed, "Cloudflare Zone 查询失败");
            }

            const auto& zones = parsed->get<"result">();
            if (zones) {
                for (const auto& zone : *zones) {
                    const auto& id = zone.get<"id">();
                    const auto& name = zone.get<"name">();
                    const auto& status = zone.get<"status">();
                    if (id && name && status && status->view() == "active" &&
                        name->view() == candidate) {
                        co_return CloudflareZone{std::string(id->view()), std::string(name->view()),
                                                 std::string(status->view())};
                    }
                }
            }
            candidate.erase(0, candidate.find('.') + 1);
        }
        throw CloudflareError(CloudflareErrorCode::zoneNotFound,
                              "Cloudflare 中未找到该域名对应的活动 Zone");
    }

    template <typename Runtime>
    ruvia::Task<CloudflareRecordPage>
    listRecords(Runtime& c, std::string_view token, std::string_view zoneId, std::int64_t page,
                std::int64_t pageSize, const std::optional<std::string>& keyword) const {
        auto target = "/client/v4/zones/" + std::string(zoneId) +
                      "/dns_records?order=name&direction=asc&page=" + std::to_string(page) +
                      "&per_page=" + std::to_string(pageSize);
        if (keyword && !keyword->empty()) {
            target += "&search=" + percentEncode(*keyword);
        }
        const auto response = co_await send(c, ruvia::HttpKnownMethod::kGet, target, token);
        const std::optional<CloudflareRecordListEnvelope> parsed =
            ruvia::fromJson<CloudflareRecordListEnvelope>(response.body(),
                                                          {.resource = c.resource()});
        if (!response.status().isSuccessful() || !parsed || !success(*parsed)) {
            fail(parsed, CloudflareErrorCode::dnsFailed, "Cloudflare DNS 记录查询失败");
        }

        CloudflareRecordPage result;
        const auto& records = parsed->get<"result">();
        if (records) {
            result.list.reserve(records->size());
            for (const auto& record : *records) {
                const auto& id = record.get<"id">();
                const auto& type = record.get<"type">();
                const auto& name = record.get<"name">();
                const auto& content = record.get<"content">();
                const auto& ttl = record.get<"ttl">();
                if (!id || !type || !name || !content || !ttl) {
                    continue;
                }
                CloudflareRecord item{std::string(id->view()),
                                      std::string(type->view()),
                                      std::string(name->view()),
                                      std::string(content->view()),
                                      *ttl,
                                      std::nullopt,
                                      false,
                                      {}};
                const auto& priority = record.get<"priority">();
                if (priority) {
                    item.priority = *priority;
                }
                const auto& proxiedValue = record.get<"proxied">();
                if (proxiedValue) {
                    item.proxied = static_cast<bool>(*proxiedValue);
                }
                const auto& modifiedOn = record.get<"modifiedOn">();
                if (modifiedOn) {
                    item.modifiedAt = std::string(modifiedOn->view());
                }
                result.list.push_back(std::move(item));
            }
        }
        const auto& info = parsed->get<"resultInfo">();
        result.total = static_cast<std::int64_t>(result.list.size());
        result.totalPages = 1;
        if (info) {
            const auto& totalCount = info->get<"totalCount">();
            if (totalCount) {
                result.total = static_cast<std::int64_t>(*totalCount);
            }
            const auto& totalPages = info->get<"totalPages">();
            if (totalPages) {
                result.totalPages = static_cast<std::int64_t>(*totalPages);
            }
        }
        co_return result;
    }

    template <typename Runtime>
    ruvia::Task<std::vector<CloudflareRecord>> listAllRecords(Runtime& c, std::string_view token,
                                                              std::string_view zoneId) const {
        std::vector<CloudflareRecord> result;
        std::int64_t page = 1;
        std::int64_t totalPages = 1;
        do {
            auto current = co_await listRecords(c, token, zoneId, page, 100, std::nullopt);
            result.insert(result.end(), std::make_move_iterator(current.list.begin()),
                          std::make_move_iterator(current.list.end()));
            totalPages = current.totalPages;
            ++page;
        } while (page <= totalPages);
        co_return result;
    }

    template <typename Runtime>
    ruvia::Task<std::string>
    reconcileRecord(Runtime& c, std::string_view token, std::string_view zoneId,
                    std::string_view remoteRecordId, std::string_view type, std::string_view name,
                    std::string_view content, std::int64_t ttl,
                    std::optional<std::int64_t> priority, bool proxied,
                    const std::vector<CloudflareRecord>& remoteRecords) const {
        if (!remoteRecordId.empty()) {
            const auto byId = std::find_if(
                remoteRecords.begin(), remoteRecords.end(),
                [remoteRecordId](const auto& record) { return record.id == remoteRecordId; });
            if (byId != remoteRecords.end()) {
                if (recordMatches(*byId, type, name, content, ttl, priority, proxied)) {
                    co_return byId->id;
                }
                co_return co_await updateRecord(c, token, zoneId, byId->id, type, name, content,
                                                ttl, priority, proxied);
            }
        }

        const CloudflareRecord* exact = nullptr;
        for (const auto& record : remoteRecords) {
            if (record.type != type || !dnsNameEquals(record.name, name) ||
                !recordContentEquals(type, record.content, content)) {
                continue;
            }
            if (exact && exact->id != record.id) {
                throw CloudflareError(CloudflareErrorCode::recordConflict,
                                      "Cloudflare 中存在多条相同 DNS 记录，无法安全接管");
            }
            exact = &record;
        }
        if (exact) {
            if (recordMatches(*exact, type, name, content, ttl, priority, proxied)) {
                co_return exact->id;
            }
            co_return co_await updateRecord(c, token, zoneId, exact->id, type, name, content, ttl,
                                            priority, proxied);
        }

        co_return co_await createOrAdoptRecord(c, token, zoneId, type, name, content, ttl, priority,
                                               proxied);
    }

    template <typename Runtime>
    ruvia::Task<std::string>
    createOrAdoptRecord(Runtime& c, std::string_view token, std::string_view zoneId,
                        std::string_view type, std::string_view name, std::string_view content,
                        std::int64_t ttl, std::optional<std::int64_t> priority,
                        bool proxied) const {
        auto existingId = co_await findExactRecordId(c, token, zoneId, type, name, content);
        if (existingId) {
            co_return co_await updateRecord(c, token, zoneId, *existingId, type, name, content, ttl,
                                            priority, proxied);
        }

        bool duplicate = false;
        try {
            co_return co_await saveRecord(c, ruvia::HttpKnownMethod::kPost,
                                          "/client/v4/zones/" + std::string(zoneId) +
                                              "/dns_records",
                                          token, type, name, content, ttl, priority, proxied);
        } catch (const CloudflareError& error) {
            if (error.code() != CloudflareErrorCode::recordConflict) {
                throw;
            }
            duplicate = true;
        }

        if (duplicate) {
            existingId = co_await findExactRecordId(c, token, zoneId, type, name, content);
            if (existingId) {
                co_return co_await updateRecord(c, token, zoneId, *existingId, type, name, content,
                                                ttl, priority, proxied);
            }
        }
        throw CloudflareError(CloudflareErrorCode::recordConflict,
                              "Cloudflare DNS 记录冲突，未找到可安全接管的相同记录");
    }

    template <typename Runtime>
    ruvia::Task<std::string>
    updateRecord(Runtime& c, std::string_view token, std::string_view zoneId,
                 std::string_view recordId, std::string_view type, std::string_view name,
                 std::string_view content, std::int64_t ttl, std::optional<std::int64_t> priority,
                 bool proxied) const {
        co_return co_await saveRecord(c, ruvia::HttpKnownMethod::kPut,
                                      "/client/v4/zones/" + std::string(zoneId) + "/dns_records/" +
                                          std::string(recordId),
                                      token, type, name, content, ttl, priority, proxied);
    }

    template <typename Runtime>
    ruvia::Task<void> deleteRecord(Runtime& c, std::string_view token, std::string_view zoneId,
                                   std::string_view recordId) const {
        const auto target =
            "/client/v4/zones/" + std::string(zoneId) + "/dns_records/" + std::string(recordId);
        const auto response = co_await send(c, ruvia::HttpKnownMethod::kDelete, target, token);
        if (response.status() == ruvia::http_status::kNotFound) {
            co_return;
        }
        const std::optional<CloudflareDeleteEnvelope> parsed =
            ruvia::fromJson<CloudflareDeleteEnvelope>(response.body(), {.resource = c.resource()});
        if (!response.status().isSuccessful() || !parsed || !success(*parsed)) {
            fail(parsed, CloudflareErrorCode::dnsFailed, "Cloudflare DNS 记录删除失败");
        }
        co_return;
    }

  private:
    static constexpr std::int64_t kRecordAlreadyExistsCode{81057};

    template <typename Runtime>
    ruvia::Task<service::outbound_http::BufferedResponse>
    send(Runtime& c, ruvia::HttpKnownMethod method, std::string_view target, std::string_view token,
         std::optional<std::string_view> body = std::nullopt) const {
        auto&& client = c.httpClient(service::config::kCloudflareOriginAlias);
        service::utils::SensitiveString authorization("Bearer " + std::string(token));
        std::array<ruvia::HttpHeaderView, 3> headers{
            ruvia::HttpHeaderView{"authorization", authorization.view()},
            ruvia::HttpHeaderView{"accept", "application/json"},
            ruvia::HttpHeaderView{"content-type", "application/json"},
        };
        const auto headerCount = body ? headers.size() : headers.size() - 1;
        try {
            co_return co_await service::outbound_http::sendBuffered(
                client,
                {
                    .method = ruvia::knownHttpMethodToken(method),
                    .target = target,
                    .headers = std::span(headers).first(headerCount),
                    .content = body ? ruvia::HttpClientRequestContentView::bytes(*body)
                                    : ruvia::HttpClientRequestContentView::none(),
                },
                {.timeout = std::chrono::seconds(10), .stopToken = c.stopToken()});
        } catch (const ruvia::HttpClientError& error) {
            using Code = ruvia::HttpClientError::Code;
            switch (error.code()) {
            case Code::kTimeout:
                throw CloudflareError(CloudflareErrorCode::upstreamFailed,
                                      "Cloudflare API 请求超时（10 秒）");
            case Code::kResolveFailed:
                throw CloudflareError(CloudflareErrorCode::upstreamFailed,
                                      "Cloudflare API 域名解析失败");
            case Code::kConnectFailed:
                throw CloudflareError(CloudflareErrorCode::upstreamFailed,
                                      "Cloudflare API 连接失败");
            case Code::kTlsFailed:
                throw CloudflareError(CloudflareErrorCode::upstreamFailed,
                                      "Cloudflare API TLS 连接失败");
            case Code::kResponseTooLarge:
                throw CloudflareError(CloudflareErrorCode::upstreamFailed,
                                      "Cloudflare API 响应超过大小限制");
            default:
                throw CloudflareError(CloudflareErrorCode::upstreamFailed,
                                      "Cloudflare API 请求失败");
            }
        }
    }

    template <typename Runtime>
    ruvia::Task<std::string> saveRecord(Runtime& c, ruvia::HttpKnownMethod method,
                                        std::string target, std::string_view token,
                                        std::string_view type, std::string_view name,
                                        std::string_view content, std::int64_t ttl,
                                        std::optional<std::int64_t> priority, bool proxied) const {
        CloudflareSaveRecordPayload payload({.resource = c.resource()});
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
        const auto json = ruvia::toJson(payload, {.resource = c.resource()});
        const auto response = co_await send(c, method, target, token, json);
        if (response.status() == ruvia::http_status::kNotFound) {
            throw CloudflareError(CloudflareErrorCode::recordNotFound, "Cloudflare DNS 记录不存在");
        }
        const std::optional<CloudflareRecordEnvelope> parsed =
            ruvia::fromJson<CloudflareRecordEnvelope>(response.body(), {.resource = c.resource()});
        if (!response.status().isSuccessful() || !parsed || !success(*parsed)) {
            if (parsed) {
                fail(parsed, CloudflareErrorCode::dnsFailed, "Cloudflare DNS 记录保存失败");
            }
            const std::optional<CloudflareErrorEnvelope> errorEnvelope =
                ruvia::fromJson<CloudflareErrorEnvelope>(response.body(),
                                                         {.resource = c.resource()});
            fail(errorEnvelope, CloudflareErrorCode::dnsFailed, "Cloudflare DNS 记录保存失败");
        }
        const auto& result = parsed->get<"result">();
        if (!result) {
            throw CloudflareError(CloudflareErrorCode::dnsFailed,
                                  "Cloudflare DNS 记录保存响应缺少记录 ID");
        }
        const auto& id = result->get<"id">();
        if (!id) {
            throw CloudflareError(CloudflareErrorCode::dnsFailed,
                                  "Cloudflare DNS 记录保存响应缺少记录 ID");
        }
        co_return std::string(id->view());
    }

    template <typename Runtime>
    ruvia::Task<std::optional<std::string>>
    findExactRecordId(Runtime& c, std::string_view token, std::string_view zoneId,
                      std::string_view type, std::string_view name,
                      std::string_view content) const {
        const auto target = "/client/v4/zones/" + std::string(zoneId) +
                            "/dns_records?match=all&type=" + percentEncode(type) +
                            "&name.exact=" + percentEncode(name) +
                            "&content.exact=" + percentEncode(content) + "&per_page=100";
        const auto response = co_await send(c, ruvia::HttpKnownMethod::kGet, target, token);
        const std::optional<CloudflareRecordListEnvelope> parsed =
            ruvia::fromJson<CloudflareRecordListEnvelope>(response.body(),
                                                          {.resource = c.resource()});
        if (!response.status().isSuccessful() || !parsed || !success(*parsed)) {
            fail(parsed, CloudflareErrorCode::dnsFailed, "Cloudflare DNS 相同记录查询失败");
        }

        std::optional<std::string> result;
        const auto& records = parsed->get<"result">();
        if (!records) {
            co_return result;
        }
        for (const auto& record : *records) {
            const auto& id = record.get<"id">();
            const auto& recordType = record.get<"type">();
            const auto& recordName = record.get<"name">();
            const auto& recordContent = record.get<"content">();
            if (!id || !recordType || !recordName || !recordContent || recordType->view() != type ||
                !dnsNameEquals(recordName->view(), name) ||
                !recordContentEquals(type, recordContent->view(), content)) {
                continue;
            }
            if (result && *result != id->view()) {
                throw CloudflareError(CloudflareErrorCode::recordConflict,
                                      "Cloudflare 中存在多条相同 DNS 记录，无法安全接管");
            }
            result = std::string(id->view());
        }
        co_return result;
    }

    template <typename Envelope> [[nodiscard]] static bool success(const Envelope& envelope) {
        const auto& value = envelope.template get<"success">();
        return value && static_cast<bool>(*value);
    }

    template <typename Envelope>
    [[nodiscard]] static std::string message(const Envelope& envelope, std::string_view fallback) {
        const auto& errors = envelope.template get<"errors">();
        if (errors && !errors->empty()) {
            const auto& code = errors->front().template get<"code">();
            if (code && static_cast<std::int64_t>(*code) == kRecordAlreadyExistsCode) {
                return "Cloudflare 中已存在相同 DNS 记录";
            }
            const auto& value = errors->front().template get<"message">();
            if (value && !value->view().empty()) {
                return std::string(value->view());
            }
        }
        return std::string(fallback);
    }

    template <typename Envelope>
    [[noreturn]] static void fail(const std::optional<Envelope>& envelope, CloudflareErrorCode code,
                                  std::string_view fallback) {
        auto resolvedCode = code;
        if (envelope) {
            const auto& errors = envelope->template get<"errors">();
            if (errors && !errors->empty()) {
                const auto& apiCode = errors->front().template get<"code">();
                if (apiCode) {
                    const auto value = static_cast<std::int64_t>(*apiCode);
                    if (value == 10000) {
                        resolvedCode = CloudflareErrorCode::authorizationFailed;
                    } else if (value == kRecordAlreadyExistsCode) {
                        resolvedCode = CloudflareErrorCode::recordConflict;
                    }
                }
            }
        }
        throw CloudflareError(resolvedCode,
                              envelope ? message(*envelope, fallback) : std::string(fallback));
    }

    [[nodiscard]] static std::string normalizeDomain(std::string_view input) {
        std::string result(input);
        std::transform(result.begin(), result.end(), result.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return result;
    }

    [[nodiscard]] static bool dnsNameEquals(std::string_view left, std::string_view right) {
        auto normalizedLeft = normalizeDomain(left);
        auto normalizedRight = normalizeDomain(right);
        if (normalizedLeft.ends_with('.')) {
            normalizedLeft.pop_back();
        }
        if (normalizedRight.ends_with('.')) {
            normalizedRight.pop_back();
        }
        return normalizedLeft == normalizedRight;
    }

    [[nodiscard]] static bool ipAddressEquals(std::string_view left, std::string_view right) {
        std::error_code leftError;
        std::error_code rightError;
        const auto leftAddress = asio::ip::make_address(left, leftError);
        const auto rightAddress = asio::ip::make_address(right, rightError);
        return !leftError && !rightError && leftAddress == rightAddress;
    }

    [[nodiscard]] static bool recordContentEquals(std::string_view type, std::string_view left,
                                                  std::string_view right) {
        if (type == "A" || type == "AAAA") {
            return ipAddressEquals(left, right);
        }
        if (type == "CNAME" || type == "MX") {
            return dnsNameEquals(left, right);
        }
        return left == right;
    }

    [[nodiscard]] static bool recordMatches(const CloudflareRecord& remote, std::string_view type,
                                            std::string_view name, std::string_view content,
                                            std::int64_t ttl, std::optional<std::int64_t> priority,
                                            bool proxied) {
        if (remote.type != type || !dnsNameEquals(remote.name, name) ||
            !recordContentEquals(type, remote.content, content) || remote.ttl != ttl) {
            return false;
        }
        if (type == "MX" && remote.priority != priority) {
            return false;
        }
        if ((type == "A" || type == "AAAA" || type == "CNAME") && remote.proxied != proxied) {
            return false;
        }
        return true;
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
};

inline const CloudflareClient& cloudflareClient() {
    static const CloudflareClient client;
    return client;
}

} // namespace service::dns
