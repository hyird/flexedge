#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/context.h>
#include <ruvia/web/db/db.h>

#include <cstdint>
#include <string>

#include "service/domains/provider/certificate_provider_types.h"
#include "service/features/certificate/provider_config_mapper.h"

namespace service::provider {

class certificate_provider_read_service final {
public:
    ruvia::task<ruvia::array<certificate_provider_dto>> list(auto& c,
        const std::string& tenant_id) const {
        const auto rows = co_await c.db().query(
            "SELECT id, revision, provider, config::text, status, TO_CHAR(last_verified_at, "
            "'YYYY-MM-DD\"T\"HH24:MI:SS.USOF'), last_error, TO_CHAR(created_at, "
            "'YYYY-MM-DD\"T\"HH24:MI:SS.USOF'), TO_CHAR(updated_at, "
            "'YYYY-MM-DD\"T\"HH24:MI:SS.USOF') FROM sys_provider WHERE tenant_id = $1 AND "
            "kind = 'certificate' AND deleted_at IS NULL ORDER BY sort DESC",
            tenant_id);
        ruvia::array<certificate_provider_dto> result({.resource_ = c.pool()});
        for (const auto& row : rows) {
            fill(c, result.emplace_back(ruvia::model_options{.resource_ = c.pool()}), row);
        }
        co_return result;
    }

private:
    template <typename row_type>
    static void fill(auto& c, certificate_provider_dto& item, const row_type& row) {
        const auto config = service::certificate_issuance::parse_certificate_provider_config(
            row[3].value().value_or("{}"), c.pool());
        item.set<"id">(row[0].value().value_or(""));
        item.set<"revision">(row[1].template as<std::int64_t>().value_or(1));
        item.set<"provider">(row[2].value().value_or(""));
        item.set<"credential_mode">(config.credential_mode_);
        item.set<"status">(row[4].value().value_or(""));
        item.set<"created_at">(row[7].value().value_or(""));
        item.set<"updated_at">(row[8].value().value_or(""));
        if (config.account_email_) {
            item.set<"account_email">(*config.account_email_);
        }
        if (config.hint_) {
            item.set<"access_key_hint">(*config.hint_);
        }
        if (const auto& last_verified_at = row[5].value()) {
            item.set<"last_verified_at">(*last_verified_at);
        }
        if (const auto& last_error = row[6].value()) {
            item.set<"last_error">(*last_error);
        }
    }
};

inline const certificate_provider_read_service& get_certificate_provider_read_service() {
    static const certificate_provider_read_service service;
    return service;
}

}  // namespace service::provider
