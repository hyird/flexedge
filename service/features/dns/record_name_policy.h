#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

#include "service/features/dns/registry.h"

namespace service::dns {

class record_name_policy final {
public:
    explicit record_name_policy(dns_provider_kind kind)
        : kind_(kind) {}
    [[nodiscard]] std::string remote_record_name(std::string_view name,
        std::string_view domain) const {
        std::string result(name);
        if (result.ends_with('.')) {
            result.pop_back();
        }
        if (kind_ == dns_provider_kind::cloudflare) {
            if (result == "@") {
                return std::string(domain);
            }
            if (dns_name_equals(result, domain) || is_subdomain(result, domain)) {
                return result;
            }
            return result + "." + std::string(domain);
        }
        if (result == "@" || dns_name_equals(result, domain)) {
            return "@";
        }
        if (is_subdomain(result, domain)) {
            result.resize(result.size() - domain.size() - 1);
        }
        return result.empty() ? "@" : result;
    }

    [[nodiscard]] std::string local_record_name(std::string_view name,
        std::string_view domain) const {
        if (kind_ == dns_provider_kind::aliyun) {
            return name.empty() ? "@" : std::string(name);
        }
        std::string result(name);
        if (result.ends_with('.')) {
            result.pop_back();
        }
        if (dns_name_equals(result, domain)) {
            return "@";
        }
        if (is_subdomain(result, domain)) {
            result.resize(result.size() - domain.size() - 1);
        }
        return result.empty() ? "@" : result;
    }

    static bool dns_name_equals(std::string_view left, std::string_view right) {
        return left.size() == right.size() &&
               std::equal(left.begin(), left.end(), right.begin(), [](char lhs, char rhs) {
                   return std::tolower(static_cast<unsigned char>(lhs)) ==
                          std::tolower(static_cast<unsigned char>(rhs));
               });
    }

    static bool is_subdomain(std::string_view name, std::string_view domain) {
        return name.size() > domain.size() && name.ends_with(domain) &&
               name[name.size() - domain.size() - 1] == '.';
    }

private:
    dns_provider_kind kind_;
};

}  // namespace service::dns
