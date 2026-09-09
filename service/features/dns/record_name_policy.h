#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include "service/features/dns/registry.h"

namespace service::dns {

class RecordNamePolicy final {
 public:
    explicit RecordNamePolicy(DnsProviderKind kind) : kind_(kind) {}
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

 private:
    DnsProviderKind kind_;
};

} // namespace service::dns
