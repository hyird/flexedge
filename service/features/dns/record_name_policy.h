#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace service::dns {

// How a provider names records: fully qualified ("www.example.com") or
// relative to the zone ("www", "@" for the apex). Each client declares its own.
class record_name_policy final {
public:
    explicit constexpr record_name_policy(bool fully_qualified)
        : fully_qualified_(fully_qualified) {}

    [[nodiscard]] std::string remote_record_name(std::string_view name,
        std::string_view domain) const {
        std::string result(name);
        if (result.ends_with('.')) {
            result.pop_back();
        }
        if (fully_qualified_) {
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
        if (!fully_qualified_) {
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
    bool fully_qualified_;
};

}  // namespace service::dns
