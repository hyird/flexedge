#pragma once

#include <cstdint>
#include <stdexcept>
#include <utility>

#include <ruvia/web/Context.h>

#include "service/domains/certificate/certificate.types.h"
#include "service/features/certificate/config_mapper.h"
#include "service/features/certificate_material/model.h"

namespace service::certificate {

template <typename Row>
inline void fillCertificate(auto& c, CertificateDto& item, const Row& row) {
    const auto config = service::certificate_issuance::parseConfigStored(
        row[2].value().value_or("{}"), {.resource = c.resource()});
    if (!config) {
        throw std::runtime_error("stored certificate config is incomplete");
    }
    const auto material = service::certificate_material::parseStored(row[4].value().value_or("{}"),
                                                                     {.resource = c.resource()});
    if (!material) {
        throw std::runtime_error("stored certificate material is invalid");
    }
    auto configDto = service::certificate_issuance::toOutput(*config, {.resource = c.resource()});
    item.set<"id">(row[0].value().value_or(""));
    item.set<"revision">(row[1].template as<std::int64_t>().value_or(1));
    item.set<"issuer">(row[15].value().value_or("") == "zerossl" ? "ZeroSSL" : "Let's Encrypt");
    item.set<"status">(row[3].value().value_or(""));
    item.set<"config">(std::move(configDto));
    item.set<"createdAt">(row[7].value().value_or(""));
    item.set<"updatedAt">(row[8].value().value_or(""));
    item.set<"dnsZoneId">(row[9].value().value_or(""));
    item.set<"dnsZoneDomain">(row[10].value().value_or(""));
    item.set<"certificateProviderId">(row[14].value().value_or(""));
    item.set<"certificateProvider">(row[15].value().value_or(""));
    item.set<"websiteCount">(row[18].template as<std::int64_t>().value_or(0));
    item.set<"usable">(row[19].template as<bool>().value_or(false));
    if (material->notBefore)
        item.set<"notBefore">(*material->notBefore);
    if (const auto& expiresAt = row[5].value())
        item.set<"expiresAt">(*expiresAt);
    if (const auto& lastError = row[6].value())
        item.set<"lastError">(*lastError);
    if (row[11].value())
        item.set<"remainingDays">(row[11].template as<std::int64_t>().value_or(0));
    if (material->serialNumber)
        item.set<"serialNumber">(*material->serialNumber);
    if (material->fingerprintSha256)
        item.set<"fingerprintSha256">(*material->fingerprintSha256);
    if (material->lastIssuedAt)
        item.set<"lastIssuedAt">(*material->lastIssuedAt);
    if (const auto& syncStatus = row[16].value())
        item.set<"syncStatus">(*syncStatus);
    if (row[17].value())
        item.set<"syncCountFails">(row[17].template as<std::int64_t>().value_or(0));
    auto& domains = item.ensure<"domains">();
    if (const auto& primaryDomain = row[12].value()) {
        domains.emplace_back(*primaryDomain, ruvia::ModelOptions{.resource = c.resource()});
    }
    if (const auto& secondaryDomain = row[13].value()) {
        domains.emplace_back(*secondaryDomain, ruvia::ModelOptions{.resource = c.resource()});
    }
}

} // namespace service::certificate
