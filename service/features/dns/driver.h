#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <asio/ip/address.hpp>

#include <ruvia/core/Task.h>
#include <ruvia/web/Context.h>

#include "service/features/dns/aliyun.h"
#include "service/features/dns/cloudflare.h"
#include "service/features/dns/record_reconciliation.h"
#include "service/features/dns/registry.h"

namespace service::dns {

struct ProviderZone final {
    std::string id;
    std::string name;
    std::string status;
};

struct ProviderRecord final {
    std::string id;
    std::string type;
    std::string name;
    std::string content;
    std::int64_t ttl;
    std::optional<std::int64_t> priority;
    bool proxied;
    std::string lineCode;
};

struct ProviderLine final {
    std::string code;
    std::string name;
    std::string displayName;
};

class DnsProviderDriver final {
  public:
    explicit DnsProviderDriver(std::string_view provider)
        : kind_(requireDnsProvider(provider).kind) {}

    [[nodiscard]] static bool supports(std::string_view provider) noexcept {
        return findDnsProvider(provider) != nullptr;
    }

    [[nodiscard]] bool supportsRoutingLines() const noexcept {
        return descriptor().supportsRoutingLines;
    }

    [[nodiscard]] std::int64_t minimumRecordTtl() const noexcept {
        return kind_ == DnsProviderKind::aliyun ? 600 : 1;
    }

    template <typename Runtime>
    ruvia::Task<std::vector<ProviderZone>> verify(Runtime& c, std::string_view accountId,
                                                  std::string_view secret) const {
        std::vector<ProviderZone> result;
        if (kind_ == DnsProviderKind::cloudflare) {
            const auto zones = co_await cloudflareClient().verifyToken(c, secret, accountId);
            result.reserve(zones.size());
            for (const auto& zone : zones) {
                result.push_back({zone.id, zone.name, zone.status});
            }
        } else {
            const auto zones = co_await aliyunClient().verifyAccessKey(c, accountId, secret);
            result.reserve(zones.size());
            for (const auto& zone : zones) {
                result.push_back({zone.id, zone.name, zone.status});
            }
        }
        co_return result;
    }

    template <typename Runtime>
    ruvia::Task<ProviderZone> findZone(Runtime& c, std::string_view accountId,
                                       std::string_view secret, std::string_view domain) const {
        if (kind_ == DnsProviderKind::cloudflare) {
            const auto zone = co_await cloudflareClient().findZone(c, secret, accountId, domain);
            co_return ProviderZone{zone.id, zone.name, zone.status};
        }
        const auto zones = co_await aliyunClient().listDomains(c, accountId, secret);
        const auto found = std::find_if(zones.begin(), zones.end(), [domain](const auto& zone) {
            return dnsNameEquals(zone.name, domain);
        });
        if (found == zones.end()) {
            throw AliyunError(AliyunErrorCode::domainNotFound, "阿里云 DNS 中未找到该托管域名");
        }
        co_return ProviderZone{found->id, found->name, found->status};
    }

    template <typename Runtime>
    ruvia::Task<std::vector<ProviderRecord>>
    listRecords(Runtime& c, std::string_view accountId, std::string_view secret,
                std::string_view zoneId, std::string_view domain) const {
        std::vector<ProviderRecord> result;
        if (kind_ == DnsProviderKind::cloudflare) {
            const auto records = co_await cloudflareClient().listAllRecords(c, secret, zoneId);
            result.reserve(records.size());
            for (const auto& record : records) {
                result.push_back({record.id, record.type, record.name, record.content, record.ttl,
                                  record.priority, record.proxied, "default"});
            }
        } else {
            const auto records =
                co_await aliyunClient().listAllRecords(c, accountId, secret, domain);
            result.reserve(records.size());
            for (const auto& record : records) {
                result.push_back({record.id, record.type, record.rr, record.value, record.ttl,
                                  record.priority, false, record.line});
            }
        }
        co_return result;
    }

    template <typename Runtime>
    ruvia::Task<std::vector<ProviderLine>> listLines(Runtime& c, std::string_view accountId,
                                                     std::string_view secret,
                                                     std::string_view domain) const {
        if (kind_ == DnsProviderKind::cloudflare) {
            co_return std::vector<ProviderLine>{{"default", "默认", "默认"}};
        }
        const auto lines = co_await aliyunClient().listSupportLines(c, accountId, secret, domain);
        std::vector<ProviderLine> result;
        result.reserve(lines.size());
        for (const auto& line : lines) {
            result.push_back({line.code, line.name, line.displayName});
        }
        co_return result;
    }

    template <typename Runtime>
    ruvia::Task<std::string>
    reconcileRecord(Runtime& c, std::string_view accountId, std::string_view secret,
                    std::string_view zoneId, std::string_view domain,
                    std::string_view remoteRecordId, std::string_view type,
                    std::string_view localName, std::string_view content, std::int64_t ttl,
                    std::optional<std::int64_t> priority, bool proxied, std::string_view lineCode,
                    const std::vector<ProviderRecord>& remoteRecords) const {
        const auto name = remoteRecordName(localName, domain);
        const auto plan = planRecordReconciliation(
            remoteRecords, remoteRecordId,
            [this, type, name, content, lineCode](const ProviderRecord& record) {
                return recordHasSameIdentity(record, type, name, content, lineCode);
            },
            [this, type, name, content, ttl, priority, proxied,
             lineCode](const ProviderRecord& record) {
                return recordMatchesDesired(record, type, name, content, ttl, priority, proxied,
                                            lineCode);
            },
            [this] {
                if (kind_ == DnsProviderKind::cloudflare) {
                    throw CloudflareError(CloudflareErrorCode::recordConflict,
                                          "Cloudflare 中存在多条相同 DNS 记录，无法安全接管");
                }
                throw AliyunError(AliyunErrorCode::recordConflict,
                                  "阿里云 DNS 中存在多条相同线路解析记录，无法安全接管");
            });
        if (plan.action == RecordReconciliationAction::reuse) {
            co_return plan.record->id;
        }
        if (kind_ == DnsProviderKind::cloudflare) {
            if (plan.action == RecordReconciliationAction::update) {
                co_return co_await cloudflareClient().updateRecord(c, secret, zoneId,
                                                                   plan.record->id, type, name,
                                                                   content, ttl, priority, proxied);
            }
            co_return co_await cloudflareClient().createOrAdoptRecord(
                c, secret, zoneId, type, name, content, ttl, priority, proxied);
        }
        if (plan.action == RecordReconciliationAction::update) {
            co_return co_await aliyunClient().updateRecord(c, accountId, secret, plan.record->id,
                                                           domain, type, name, content, ttl,
                                                           priority, lineCode);
        }
        co_return co_await aliyunClient().createRecord(c, accountId, secret, domain, type, name,
                                                       content, ttl, priority, lineCode);
    }

    template <typename Runtime>
    ruvia::Task<void> deleteRecord(Runtime& c, std::string_view accountId, std::string_view secret,
                                   std::string_view zoneId, std::string_view recordId) const {
        if (kind_ == DnsProviderKind::cloudflare) {
            co_await cloudflareClient().deleteRecord(c, secret, zoneId, recordId);
        } else {
            co_await aliyunClient().deleteRecord(c, accountId, secret, recordId);
        }
        co_return;
    }

    [[nodiscard]] std::string remoteRecordName(std::string_view name,
                                               std::string_view domain) const {
        std::string result(name);
        if (result.ends_with('.')) {
            result.pop_back();
        }
        if (kind_ == DnsProviderKind::cloudflare) {
            if (result == "@") {
                return std::string(domain);
            }
            if (dnsNameEquals(result, domain) || isSubdomain(result, domain)) {
                return result;
            }
            return result + "." + std::string(domain);
        }
        if (result == "@" || dnsNameEquals(result, domain)) {
            return "@";
        }
        if (isSubdomain(result, domain)) {
            result.resize(result.size() - domain.size() - 1);
        }
        return result.empty() ? "@" : result;
    }

    [[nodiscard]] std::string localRecordName(std::string_view name,
                                              std::string_view domain) const {
        if (kind_ == DnsProviderKind::aliyun) {
            return name.empty() ? "@" : std::string(name);
        }
        std::string result(name);
        if (result.ends_with('.')) {
            result.pop_back();
        }
        if (dnsNameEquals(result, domain)) {
            return "@";
        }
        if (isSubdomain(result, domain)) {
            result.resize(result.size() - domain.size() - 1);
        }
        return result.empty() ? "@" : result;
    }

  private:
    [[nodiscard]] const DnsProviderDescriptor& descriptor() const noexcept {
        for (const auto& provider : kDnsProviders) {
            if (provider.kind == kind_) {
                return provider;
            }
        }
        std::terminate();
    }

    static bool dnsNameEquals(std::string_view left, std::string_view right) {
        return left.size() == right.size() &&
               std::equal(left.begin(), left.end(), right.begin(), [](char lhs, char rhs) {
                   return std::tolower(static_cast<unsigned char>(lhs)) ==
                          std::tolower(static_cast<unsigned char>(rhs));
               });
    }

    static bool isSubdomain(std::string_view name, std::string_view domain) {
        return name.size() > domain.size() && name.ends_with(domain) &&
               name[name.size() - domain.size() - 1] == '.';
    }

    [[nodiscard]] bool recordHasSameIdentity(const ProviderRecord& record, std::string_view type,
                                             std::string_view name, std::string_view content,
                                             std::string_view lineCode) const {
        if (record.type != type || !recordNameEquals(record.name, name) ||
            !recordContentEquals(type, record.content, content)) {
            return false;
        }
        return kind_ != DnsProviderKind::aliyun || record.lineCode == lineCode;
    }

    [[nodiscard]] bool recordMatchesDesired(const ProviderRecord& record, std::string_view type,
                                            std::string_view name, std::string_view content,
                                            std::int64_t ttl, std::optional<std::int64_t> priority,
                                            bool proxied, std::string_view lineCode) const {
        if (!recordHasSameIdentity(record, type, name, content, lineCode) || record.ttl != ttl) {
            return false;
        }
        if (type == "MX" && record.priority != priority) {
            return false;
        }
        if (kind_ == DnsProviderKind::cloudflare &&
            (type == "A" || type == "AAAA" || type == "CNAME")) {
            return record.proxied == proxied;
        }
        return true;
    }

    [[nodiscard]] bool recordContentEquals(std::string_view type, std::string_view left,
                                           std::string_view right) const {
        if (kind_ != DnsProviderKind::cloudflare) {
            return left == right;
        }
        if (type == "A" || type == "AAAA") {
            return ipAddressEquals(left, right);
        }
        if (type == "CNAME" || type == "MX") {
            return recordNameEquals(left, right);
        }
        return left == right;
    }

    [[nodiscard]] static bool ipAddressEquals(std::string_view left, std::string_view right) {
        std::error_code leftError;
        std::error_code rightError;
        const auto leftAddress = asio::ip::make_address(left, leftError);
        const auto rightAddress = asio::ip::make_address(right, rightError);
        return !leftError && !rightError && leftAddress == rightAddress;
    }

    [[nodiscard]] bool recordNameEquals(std::string_view left, std::string_view right) const {
        return normalizeRecordName(left, kind_ == DnsProviderKind::cloudflare) ==
               normalizeRecordName(right, kind_ == DnsProviderKind::cloudflare);
    }

    [[nodiscard]] static std::string normalizeRecordName(std::string_view input,
                                                         bool trimTrailingDot) {
        std::string result(input);
        std::transform(result.begin(), result.end(), result.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (trimTrailingDot && result.ends_with('.')) {
            result.pop_back();
        }
        return result;
    }

    DnsProviderKind kind_;
};

} // namespace service::dns
