#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/context.h>
#include <ruvia/web/db/db.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "service/common/http.h"
#include "service/domains/provider/dns_provider_error.h"
#include "service/domains/provider/dns_provider_types.h"
#include "service/features/dns/provider_config_mapper.h"

namespace service::provider {

class dns_provider_read_service final {
public:
    ruvia::task<dns_provider_page_data_dto> list(auto& c, const std::string& tenant_id,
        std::int64_t page, std::int64_t page_size,
        std::int64_t skip,
        const std::optional<std::string>& keyword,
        const std::optional<std::string>& status) const {
        std::string where =
            " FROM sys_provider provider WHERE provider.tenant_id = $1 AND provider.kind = "
            "'dns' AND provider.deleted_at IS NULL";
        std::vector<ruvia::db_value> params{ruvia::db_value{tenant_id}};
        std::optional<std::string> pattern;
        if (status) {
            where += " AND provider.status = $" + std::to_string(params.size() + 1);
            params.emplace_back(std::string_view(*status));
        }
        if (keyword) {
            pattern = "%" + service::common::escape_like_pattern(*keyword) + "%";
            const auto placeholder = "$" + std::to_string(params.size() + 1);
            where += " AND (provider.name ILIKE " + placeholder + " OR provider.account_id ILIKE " +
                     placeholder + ")";
            params.emplace_back(std::string_view(*pattern));
        }
        const auto count_rows = co_await c.db().query("SELECT COUNT(*)" + where, params);
        const auto total = count_rows.empty() ? std::int64_t{0}
                                              : count_rows.front()[0].template as<std::int64_t>().value_or(0);
        const auto rows =
            co_await c.db().query(select_columns() + where + " ORDER BY provider.sort DESC LIMIT " +
                                      std::to_string(page_size) + " OFFSET " + std::to_string(skip),
                params);

        dns_provider_page_data_dto result({.resource_ = c.pool()});
        result.template set<"total">(total);
        result.template set<"page">(page);
        result.template set<"page_size">(page_size);
        result.template set<"total_pages">(page_size > 0 ? (total + page_size - 1) / page_size : 0);
        auto& items = result.template ensure<"list">();
        for (const auto& row : rows) {
            fill(items.emplace_back(ruvia::model_options{.resource_ = c.pool()}), parse_row(c, row));
        }
        co_return result;
    }

    ruvia::task<dns_provider_dto> get(auto& c, const std::string& tenant_id,
        const std::string& id) const {
        const auto rows = co_await c.db().query(
            select_columns() +
                " FROM sys_provider provider WHERE provider.id = $1 AND "
                "provider.tenant_id = $2 AND provider.kind = 'dns' AND "
                "provider.deleted_at IS NULL LIMIT 1",
            id, tenant_id);
        if (rows.empty()) {
            service::common::throw_app_error(dns_provider_error::not_found);
        }
        dns_provider_dto result({.resource_ = c.pool()});
        fill(result, parse_row(c, rows.front()));
        co_return result;
    }

private:
    struct stored_provider final {
        std::string id_;
        std::string provider_;
        std::int64_t revision_{};
        std::string name_;
        std::string account_id_;
        std::string hint_;
        std::string status_;
        std::optional<std::string> last_verified_at_;
        std::optional<std::string> last_error_;
        std::string created_at_;
        std::string updated_at_;
        std::int64_t zone_count_{};
    };

    static std::string select_columns() {
        return "SELECT provider.id, provider.provider, provider.revision, provider.name, "
               "provider.account_id, provider.config::text, provider.status, "
               "TO_CHAR(provider.last_verified_at, 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF'), "
               "provider.last_error, TO_CHAR(provider.created_at, "
               "'YYYY-MM-DD\"T\"HH24:MI:SS.USOF'), "
               "TO_CHAR(provider.updated_at, 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF'), "
               "(SELECT COUNT(*) FROM sys_dns_zone zone WHERE zone.tenant_id = "
               "provider.tenant_id AND zone.provider_id = provider.id AND zone.deleted_at IS "
               "NULL)";
    }

    template <typename row_type>
    static stored_provider parse_row(auto& c, const row_type& row) {
        const auto config =
            service::dns::parse_dns_provider_config(row[5].value().value_or("{}"), c.pool());
        return {
            .id_ = std::string(row[0].value().value_or("")),
            .provider_ = std::string(row[1].value().value_or("")),
            .revision_ = row[2].template as<std::int64_t>().value_or(0),
            .name_ = std::string(row[3].value().value_or("")),
            .account_id_ = std::string(row[4].value().value_or("")),
            .hint_ = config.credential_hint_,
            .status_ = std::string(row[6].value().value_or("")),
            .last_verified_at_ = optional_string(row[7]),
            .last_error_ = optional_string(row[8]),
            .created_at_ = std::string(row[9].value().value_or("")),
            .updated_at_ = std::string(row[10].value().value_or("")),
            .zone_count_ = row[11].template as<std::int64_t>().value_or(0),
        };
    }

    template <typename value_type>
    static std::optional<std::string> optional_string(const value_type& value) {
        if (const auto result = value.value()) {
            return std::string(*result);
        }
        return std::nullopt;
    }

    static void fill(dns_provider_dto& item, const stored_provider& provider) {
        item.template set<"id">(provider.id_);
        item.template set<"revision">(provider.revision_);
        item.template set<"name">(provider.name_);
        item.template set<"account_id">(provider.account_id_);
        item.template set<"provider">(provider.provider_);
        item.template set<"token_hint">(provider.hint_);
        item.template set<"status">(provider.status_);
        item.template set<"created_at">(provider.created_at_);
        item.template set<"updated_at">(provider.updated_at_);
        item.template set<"zone_count">(provider.zone_count_);
        if (provider.last_verified_at_) {
            item.template set<"last_verified_at">(*provider.last_verified_at_);
        }
        if (provider.last_error_) {
            item.template set<"last_error">(*provider.last_error_);
        }
    }
};

inline const dns_provider_read_service& get_dns_provider_read_service() {
    static const dns_provider_read_service service;
    return service;
}

}  // namespace service::provider
