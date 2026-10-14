#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/context.h>

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

#include "service/features/dns/aliyun.h"
#include "service/features/dns/cloudflare.h"
#include "service/features/dns/record_model.h"
#include "service/features/dns/record_name_policy.h"
#include "service/features/dns/record_reconciliation.h"
#include "service/features/dns/registry.h"

namespace service::dns {

// Outcome of reconciling one desired record: the provider record id it now
// lives under and, when an existing provider record was reused or updated,
// that record as it was before the call.
struct reconciled_record final {
    record_reconciliation_action action_;
    std::string remote_id_;
    std::optional<provider_record> previous_;
};

class dns_provider_driver final {
public:
    explicit dns_provider_driver(std::string_view provider)
        : kind_(require_dns_provider(provider).kind_) {}

    [[nodiscard]] static bool supports(std::string_view provider) noexcept {
        return find_dns_provider(provider) != nullptr;
    }

    [[nodiscard]] bool supports_routing_lines() const noexcept {
        return descriptor().supports_routing_lines_;
    }

    [[nodiscard]] std::int64_t minimum_record_ttl() const noexcept {
        return kind_ == dns_provider_kind::aliyun ? 600 : 1;
    }

    template <typename runtime_type>
    ruvia::task<std::vector<provider_zone>> verify(runtime_type& c, service::attempt_trace::recorder& trace,
        std::string_view account_id, std::string_view secret) const {
        std::vector<provider_zone> result;
        if (kind_ == dns_provider_kind::cloudflare) {
            const auto zones = co_await get_cloudflare_client().verify_token(c, trace, secret, account_id);
            result.reserve(zones.size());
            for (const auto& zone : zones) {
                result.push_back({zone.id_, zone.name_, zone.status_});
            }
        } else {
            const auto zones = co_await get_aliyun_client().verify_access_key(c, trace, account_id, secret);
            result.reserve(zones.size());
            for (const auto& zone : zones) {
                result.push_back({zone.id_, zone.name_, zone.status_});
            }
        }
        co_return result;
    }

    template <typename runtime_type>
    ruvia::task<provider_zone> find_zone(runtime_type& c, service::attempt_trace::recorder& trace,
        std::string_view account_id, std::string_view secret, std::string_view domain) const {
        if (kind_ == dns_provider_kind::cloudflare) {
            const auto zone = co_await get_cloudflare_client().find_zone(c, trace, secret, account_id, domain);
            co_return provider_zone{zone.id_, zone.name_, zone.status_};
        }
        const auto zones = co_await get_aliyun_client().list_domains(c, trace, account_id, secret);
        const auto found = std::find_if(zones.begin(), zones.end(), [domain](const auto& zone) {
            return record_name_policy::dns_name_equals(zone.name_, domain);
        });
        if (found == zones.end()) {
            throw aliyun_error(aliyun_error_code::domain_not_found, "阿里云 DNS 中未找到该托管域名");
        }
        co_return provider_zone{found->id_, found->name_, found->status_};
    }

    template <typename runtime_type>
    ruvia::task<std::vector<provider_record>>
    list_records(runtime_type& c, service::attempt_trace::recorder& trace, std::string_view account_id,
        std::string_view secret, std::string_view zone_id, std::string_view domain) const {
        std::vector<provider_record> result;
        if (kind_ == dns_provider_kind::cloudflare) {
            const auto records = co_await get_cloudflare_client().list_all_records(c, trace, secret, zone_id);
            result.reserve(records.size());
            for (const auto& record : records) {
                result.push_back({record.id_, record.type_, record.name_, record.content_, record.ttl_,
                    record.priority_, record.proxied_, "default"});
            }
        } else {
            const auto records =
                co_await get_aliyun_client().list_all_records(c, trace, account_id, secret, domain);
            result.reserve(records.size());
            for (const auto& record : records) {
                result.push_back({record.id_, record.type_, record.rr_, record.value_, record.ttl_,
                    record.priority_, false, record.line_});
            }
        }
        co_return result;
    }

    template <typename runtime_type>
    ruvia::task<std::vector<provider_line>> list_lines(runtime_type& c, service::attempt_trace::recorder& trace,
        std::string_view account_id, std::string_view secret, std::string_view domain) const {
        if (kind_ == dns_provider_kind::cloudflare) {
            co_return std::vector<provider_line>{{"default", "默认", "默认"}};
        }
        const auto lines = co_await get_aliyun_client().list_support_lines(c, trace, account_id, secret, domain);
        std::vector<provider_line> result;
        result.reserve(lines.size());
        for (const auto& line : lines) {
            result.push_back({line.code_, line.name_, line.display_name_});
        }
        co_return result;
    }

    template <typename runtime_type>
    ruvia::task<reconciled_record>
    reconcile_record(runtime_type& c, service::attempt_trace::recorder& trace, std::string_view account_id,
        std::string_view secret,
        std::string_view zone_id, std::string_view domain,
        std::string_view remote_record_id, std::string_view type,
        std::string_view local_name, std::string_view content, std::int64_t ttl,
        std::optional<std::int64_t> priority, bool proxied, std::string_view line_code,
        const std::vector<provider_record>& remote_records) const {
        const auto name = record_names().remote_record_name(local_name, domain);
        const auto plan = plan_record_reconciliation(
            remote_records, remote_record_id,
            [this, type, name, content, line_code](const provider_record& record) {
                return record_has_same_identity(record, type, name, content, line_code);
            },
            [this, type, name, content, ttl, priority, proxied,
                line_code](const provider_record& record) {
                return record_matches_desired(record, type, name, content, ttl, priority, proxied,
                    line_code);
            },
            [this] {
                if (kind_ == dns_provider_kind::cloudflare) {
                    throw cloudflare_error(cloudflare_error_code::record_conflict,
                        "Cloudflare 中存在多条相同 DNS 记录，无法安全接管");
                }
                throw aliyun_error(aliyun_error_code::record_conflict,
                    "阿里云 DNS 中存在多条相同线路解析记录，无法安全接管");
            });
        reconciled_record result{.action_ = plan.action_, .remote_id_ = {}, .previous_ = {}};
        if (plan.record_ != nullptr) {
            result.previous_ = *plan.record_;
        }
        if (plan.action_ == record_reconciliation_action::reuse) {
            result.remote_id_ = plan.record_->id_;
            co_return result;
        }
        if (kind_ == dns_provider_kind::cloudflare) {
            if (plan.action_ == record_reconciliation_action::update) {
                result.remote_id_ = co_await get_cloudflare_client().update_record(c, trace, secret, zone_id,
                    plan.record_->id_, type, name,
                    content, ttl, priority, proxied);
                co_return result;
            }
            result.remote_id_ = co_await get_cloudflare_client().create_or_adopt_record(
                c, trace, secret, zone_id, type, name, content, ttl, priority, proxied);
            co_return result;
        }
        if (plan.action_ == record_reconciliation_action::update) {
            result.remote_id_ = co_await get_aliyun_client().update_record(c, trace, account_id, secret,
                plan.record_->id_, domain, type, name, content, ttl,
                priority, line_code);
            co_return result;
        }
        result.remote_id_ = co_await get_aliyun_client().create_record(c, trace, account_id, secret, domain,
            type, name, content, ttl, priority, line_code);
        co_return result;
    }

    template <typename runtime_type>
    ruvia::task<void> delete_record(runtime_type& c, service::attempt_trace::recorder& trace,
        std::string_view account_id, std::string_view secret, std::string_view zone_id, std::string_view record_id,
        std::string_view subject) const {
        if (kind_ == dns_provider_kind::cloudflare) {
            co_await get_cloudflare_client().delete_record(c, trace, secret, zone_id, record_id, subject);
        } else {
            co_await get_aliyun_client().delete_record(c, trace, account_id, secret, record_id, subject);
        }
        co_return;
    }

    [[nodiscard]] record_name_policy record_names() const {
        return record_name_policy(kind_);
    }

private:
    [[nodiscard]] const dns_provider_descriptor& descriptor() const noexcept {
        for (const auto& provider : dns_providers) {
            if (provider.kind_ == kind_) {
                return provider;
            }
        }
        std::terminate();
    }

    [[nodiscard]] bool record_has_same_identity(const provider_record& record, std::string_view type,
        std::string_view name, std::string_view content,
        std::string_view line_code) const {
        if (record.type_ != type || !record_name_equals(record.name_, name) ||
            !record_content_equals(type, record.content_, content)) {
            return false;
        }
        return kind_ != dns_provider_kind::aliyun || record.line_code_ == line_code;
    }

    [[nodiscard]] bool record_matches_desired(const provider_record& record, std::string_view type,
        std::string_view name, std::string_view content,
        std::int64_t ttl, std::optional<std::int64_t> priority,
        bool proxied, std::string_view line_code) const {
        if (!record_has_same_identity(record, type, name, content, line_code) || record.ttl_ != ttl) {
            return false;
        }
        if (type == "MX" && record.priority_ != priority) {
            return false;
        }
        if (kind_ == dns_provider_kind::cloudflare &&
            (type == "A" || type == "AAAA" || type == "CNAME")) {
            return record.proxied_ == proxied;
        }
        return true;
    }

    [[nodiscard]] bool record_content_equals(std::string_view type, std::string_view left,
        std::string_view right) const {
        if (kind_ != dns_provider_kind::cloudflare) {
            return left == right;
        }
        if (type == "A" || type == "AAAA") {
            return ip_address_equals(left, right);
        }
        if (type == "CNAME" || type == "MX") {
            return record_name_equals(left, right);
        }
        return left == right;
    }

    [[nodiscard]] static bool ip_address_equals(std::string_view left, std::string_view right) {
        std::error_code left_error;
        std::error_code right_error;
        const auto left_address = asio::ip::make_address(left, left_error);
        const auto right_address = asio::ip::make_address(right, right_error);
        return !left_error && !right_error && left_address == right_address;
    }

    [[nodiscard]] bool record_name_equals(std::string_view left, std::string_view right) const {
        return normalize_record_name(left, kind_ == dns_provider_kind::cloudflare) ==
               normalize_record_name(right, kind_ == dns_provider_kind::cloudflare);
    }

    [[nodiscard]] static std::string normalize_record_name(std::string_view input,
        bool trim_trailing_dot) {
        std::string result(input);
        std::transform(result.begin(), result.end(), result.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (trim_trailing_dot && result.ends_with('.')) {
            result.pop_back();
        }
        return result;
    }

    dns_provider_kind kind_;
};

}  // namespace service::dns
