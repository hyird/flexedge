#pragma once

#include <ruvia/web/context.h>

#include <cstdint>
#include <stdexcept>
#include <utility>

#include "service/domains/certificate/certificate_types.h"
#include "service/features/certificate/config_mapper.h"
#include "service/features/certificate_material/model.h"

namespace service::certificate {

template <typename row_type>
inline void fill_certificate(auto& c, certificate_dto& item, const row_type& row) {
    const auto config = service::certificate_issuance::parse_config_stored(
        row[2].value().value_or("{}"), {.resource_ = c.pool()});
    if (!config) {
        throw std::runtime_error("stored certificate config is incomplete");
    }
    const auto material = service::certificate_material::parse_stored(row[4].value().value_or("{}"),
        {.resource_ = c.pool()});
    if (!material) {
        throw std::runtime_error("stored certificate material is invalid");
    }
    auto config_dto = service::certificate_issuance::to_output(*config, {.resource_ = c.pool()});
    item.set<"id">(row[0].value().value_or(""));
    item.set<"revision">(row[1].template as<std::int64_t>().value_or(1));
    item.set<"issuer">(row[15].value().value_or("") == "zerossl" ? "ZeroSSL" : "Let's Encrypt");
    item.set<"status">(row[3].value().value_or(""));
    item.set<"config">(std::move(config_dto));
    item.set<"created_at">(row[7].value().value_or(""));
    item.set<"updated_at">(row[8].value().value_or(""));
    item.set<"dns_zone_id">(row[9].value().value_or(""));
    item.set<"dns_zone_domain">(row[10].value().value_or(""));
    item.set<"certificate_provider_id">(row[14].value().value_or(""));
    item.set<"certificate_provider">(row[15].value().value_or(""));
    item.set<"website_count">(row[18].template as<std::int64_t>().value_or(0));
    item.set<"usable">(row[19].template as<bool>().value_or(false));
    if (material->not_before_) {
        item.set<"not_before">(*material->not_before_);
    }
    if (const auto& expires_at = row[5].value()) {
        item.set<"expires_at">(*expires_at);
    }
    if (const auto& last_error = row[6].value()) {
        item.set<"last_error">(*last_error);
    }
    if (row[11].value()) {
        item.set<"remaining_days">(row[11].template as<std::int64_t>().value_or(0));
    }
    if (material->serial_number_) {
        item.set<"serial_number">(*material->serial_number_);
    }
    if (material->fingerprint_sha256_) {
        item.set<"fingerprint_sha256">(*material->fingerprint_sha256_);
    }
    if (material->last_issued_at_) {
        item.set<"last_issued_at">(*material->last_issued_at_);
    }
    if (const auto& sync_status = row[16].value()) {
        item.set<"sync_status">(*sync_status);
    }
    if (row[17].value()) {
        item.set<"sync_count_fails">(row[17].template as<std::int64_t>().value_or(0));
    }
    auto& domains = item.ensure<"domains">();
    if (const auto& primary_domain = row[12].value()) {
        domains.emplace_back(*primary_domain, ruvia::model_options{.resource_ = c.pool()});
    }
    if (const auto& secondary_domain = row[13].value()) {
        domains.emplace_back(*secondary_domain, ruvia::model_options{.resource_ = c.pool()});
    }
}

}  // namespace service::certificate
