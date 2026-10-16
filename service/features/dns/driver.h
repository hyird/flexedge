#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/context.h>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "service/features/dns/aliyun.h"
#include "service/features/dns/cloudflare.h"
#include "service/features/dns/record_model.h"
#include "service/features/dns/record_name_policy.h"
#include "service/features/dns/record_reconciliation.h"
#include "service/features/dns/registry.h"

namespace service::dns {

// Supported DNS clients share operations while keeping their own record rules.
using dns_provider_client = std::variant<cloudflare_client, aliyun_client>;

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
    explicit dns_provider_driver(std::string_view provider, service::outbound_http::quota_scope scope = {})
        : descriptor_(&require_dns_provider(provider)),
          client_(make_client(provider, std::move(scope))) {}

    [[nodiscard]] bool supports_routing_lines() const noexcept {
        return descriptor_->supports_routing_lines_;
    }

    [[nodiscard]] std::int64_t minimum_record_ttl() const noexcept {
        return std::visit([](const auto& client) { return client.minimum_record_ttl; }, client_);
    }

    [[nodiscard]] record_name_policy record_names() const noexcept {
        return std::visit([](const auto& client) { return client.record_names; }, client_);
    }

    template <typename runtime_type>
    ruvia::task<std::vector<provider_zone>> verify(runtime_type& c, service::attempt_trace::recorder& trace,
        std::string_view account_id, std::string_view secret) const {
        return std::visit(
            [&](const auto& client) { return client.verify(c, trace, account_id, secret); }, client_);
    }

    template <typename runtime_type>
    ruvia::task<provider_zone> find_zone(runtime_type& c, service::attempt_trace::recorder& trace,
        std::string_view account_id, std::string_view secret, std::string_view domain) const {
        return std::visit(
            [&](const auto& client) { return client.find_zone(c, trace, account_id, secret, domain); }, client_);
    }

    template <typename runtime_type>
    ruvia::task<std::vector<provider_record>> list_records(runtime_type& c, service::attempt_trace::recorder& trace,
        std::string_view account_id, std::string_view secret, std::string_view zone_id,
        std::string_view domain) const {
        return std::visit(
            [&](const auto& client) { return client.list_records(c, trace, account_id, secret, zone_id, domain); },
            client_);
    }

    template <typename runtime_type>
    ruvia::task<std::vector<provider_line>> list_lines(runtime_type& c, service::attempt_trace::recorder& trace,
        std::string_view account_id, std::string_view secret, std::string_view domain) const {
        return std::visit(
            [&](const auto& client) { return client.list_lines(c, trace, account_id, secret, domain); }, client_);
    }

    template <typename runtime_type>
    ruvia::task<reconciled_record>
    reconcile_record(runtime_type& c, service::attempt_trace::recorder& trace, std::string_view account_id,
        std::string_view secret, std::string_view zone_id, std::string_view domain,
        std::string_view remote_record_id, std::string_view type, std::string_view local_name,
        std::string_view content, std::int64_t ttl, std::optional<std::int64_t> priority, bool proxied,
        std::string_view line_code, const std::vector<provider_record>& remote_records) const {
        const provider_record desired{.id_ = {},
            .type_ = std::string(type),
            .name_ = record_names().remote_record_name(local_name, domain),
            .content_ = std::string(content),
            .ttl_ = ttl,
            .priority_ = priority,
            .proxied_ = proxied,
            .line_code_ = std::string(line_code)};
        co_return co_await std::visit(
            [&](const auto& client) {
                return reconcile(client, c, trace, account_id, secret, zone_id, domain, remote_record_id, desired,
                    remote_records);
            },
            client_);
    }

    template <typename runtime_type>
    ruvia::task<void> delete_record(runtime_type& c, service::attempt_trace::recorder& trace,
        std::string_view account_id, std::string_view secret, std::string_view zone_id, std::string_view record_id,
        std::string_view subject) const {
        return std::visit(
            [&](const auto& client) {
                return client.delete_record(c, trace, account_id, secret, zone_id, record_id, subject);
            },
            client_);
    }

private:
    [[nodiscard]] static dns_provider_client make_client(std::string_view provider,
        service::outbound_http::quota_scope scope) {
        if (provider == cloudflare_client::code) {
            return dns_provider_client(std::in_place_type<cloudflare_client>, std::move(scope));
        }
        if (provider == aliyun_client::code) {
            return dns_provider_client(std::in_place_type<aliyun_client>, std::move(scope));
        }
        throw std::invalid_argument("不支持的 DNS 服务商类型");
    }

    // Reuses, updates or creates the provider record for `desired`, by the
    // client's own record comparison.
    template <typename client_type, typename runtime_type>
    static ruvia::task<reconciled_record> reconcile(const client_type& client, runtime_type& c,
        service::attempt_trace::recorder& trace, std::string_view account_id, std::string_view secret,
        std::string_view zone_id, std::string_view domain, std::string_view remote_record_id,
        const provider_record& desired, const std::vector<provider_record>& remote_records) {
        const auto plan = plan_record_reconciliation(
            remote_records, remote_record_id,
            [&desired](const provider_record& record) { return client_type::same_identity(record, desired); },
            [&desired](const provider_record& record) { return client_type::matches_desired(record, desired); },
            [] { client_type::throw_duplicate_records(); });
        reconciled_record result{.action_ = plan.action_, .remote_id_ = {}, .previous_ = {}};
        if (plan.record_ != nullptr) {
            result.previous_ = *plan.record_;
        }
        if (plan.action_ == record_reconciliation_action::reuse) {
            result.remote_id_ = plan.record_->id_;
        } else if (plan.action_ == record_reconciliation_action::update) {
            result.remote_id_ = co_await client.update_record(c, trace, account_id, secret, zone_id, domain,
                plan.record_->id_, desired);
        } else {
            result.remote_id_ =
                co_await client.create_record(c, trace, account_id, secret, zone_id, domain, desired);
        }
        co_return result;
    }

    const dns_provider_descriptor* descriptor_;
    dns_provider_client client_;
};

}  // namespace service::dns
