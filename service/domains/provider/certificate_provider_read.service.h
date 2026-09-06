#pragma once

#include <cstdint>
#include <string>

#include <ruvia/core/Task.h>
#include <ruvia/web/Context.h>
#include <ruvia/web/db/Db.h>

#include "service/domains/provider/certificate_provider.types.h"
#include "service/features/certificate/provider_config.h"

namespace service::provider {

class CertificateProviderReadService final {
  public:
    ruvia::Task<ruvia::Array<CertificateProviderDto>> list(ruvia::Context& c,
                                                           const std::string& tenantId) const {
        const auto rows = co_await c.db().query(
            "SELECT id, revision, provider, config::text, status, TO_CHAR(last_verified_at, "
            "'YYYY-MM-DD\"T\"HH24:MI:SS.USOF'), last_error, TO_CHAR(created_at, "
            "'YYYY-MM-DD\"T\"HH24:MI:SS.USOF'), TO_CHAR(updated_at, "
            "'YYYY-MM-DD\"T\"HH24:MI:SS.USOF') FROM sys_provider WHERE tenant_id = $1 AND "
            "kind = 'certificate' AND deleted_at IS NULL ORDER BY sort DESC",
            tenantId);
        ruvia::Array<CertificateProviderDto> result(c.resource());
        for (const auto& row : rows) {
            fill(c, result.emplace_back(c), row);
        }
        co_return result;
    }

  private:
    template <typename Row>
    static void fill(ruvia::Context& c, CertificateProviderDto& item, const Row& row) {
        const auto config = service::certificate_issuance::parseCertificateProviderConfig(
            row[3].value().value_or("{}"), c.resource());
        item.set<"id">(row[0].value().value_or(""));
        item.set<"revision">(row[1].template as<std::int64_t>().value_or(1));
        item.set<"provider">(row[2].value().value_or(""));
        item.set<"credentialMode">(config.credentialMode);
        item.set<"status">(row[4].value().value_or(""));
        item.set<"createdAt">(row[7].value().value_or(""));
        item.set<"updatedAt">(row[8].value().value_or(""));
        if (config.accountEmail) {
            item.set<"accountEmail">(*config.accountEmail);
        }
        if (config.hint) {
            item.set<"accessKeyHint">(*config.hint);
        }
        if (const auto& lastVerifiedAt = row[5].value()) {
            item.set<"lastVerifiedAt">(*lastVerifiedAt);
        }
        if (const auto& lastError = row[6].value()) {
            item.set<"lastError">(*lastError);
        }
    }
};

inline const CertificateProviderReadService& certificateProviderReadService() {
    static const CertificateProviderReadService service;
    return service;
}

} // namespace service::provider
