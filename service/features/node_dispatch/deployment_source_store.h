#pragma once
#include <ruvia/core/task.h>
#include <ruvia/web/db/db_transaction.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "service/features/certificate_material/model.h"
#include "service/features/node_dispatch/deployment_source.h"

namespace service::node_dispatch {

inline ruvia::task<cluster_deployment_source>
load_cluster_deployment_source(ruvia::db_transaction& transaction, std::string tenant_id,
    std::string cluster_id, std::string access_domain, bool cluster_enabled) {
    cluster_deployment_source result{.cluster_id_ = std::move(cluster_id),
        .tenant_id_ = std::move(tenant_id),
        .access_domain_ = std::move(access_domain),
        .enabled_ = cluster_enabled,
        .websites_ = {},
        .certificates_by_website_ = {}};
    const auto website_rows = co_await transaction.query(
        "SELECT id, revision, status = 'enabled', config::text FROM sys_website WHERE tenant_id = "
        "$1 AND cluster_id = $2 AND deleted_at IS NULL ORDER BY sort ASC",
        result.tenant_id_, result.cluster_id_);
    result.websites_.reserve(website_rows.size());
    for (const auto& row : website_rows) {
        result.websites_.push_back({
            .id_ = std::string(row[0].value().value()),
            .revision_ = row[1].as<std::int64_t>().value(),
            .enabled_ = row[2].as<bool>().value(),
            .config_json_ = std::string(row[3].value().value()),
        });
    }

    const auto certificate_rows = co_await transaction.query(
        "SELECT binding.website_id, certificate.id, certificate.subject_alt_names[1], "
        "certificate.subject_alt_names[2], "
        "certificate.material::text FROM sys_website_certificate_binding binding INNER JOIN "
        "sys_website website ON website.tenant_id = binding.tenant_id AND website.id = "
        "binding.website_id INNER JOIN sys_certificate certificate ON certificate.tenant_id = "
        "binding.tenant_id AND certificate.id = binding.certificate_id WHERE "
        "binding.tenant_id = $1 AND website.cluster_id = $2 AND website.deleted_at IS NULL AND "
        "certificate.issued_revision > 0 AND certificate.expires_at > NOW() AND "
        "certificate.deleted_at IS NULL ORDER BY website.sort ASC, binding.position ASC",
        result.tenant_id_, result.cluster_id_);
    for (const auto& row : certificate_rows) {
        const auto material =
            service::certificate_material::parse_stored(row[4].value().value());
        if (!material || !material->certificate_chain_pem_ || !material->private_key_envelope_) {
            throw std::runtime_error("certificate release material is invalid");
        }
        std::vector<std::string> domains;
        for (const auto index : {2U, 3U}) {
            if (const auto name = row[index].value(); name && !name->empty()) {
                domains.emplace_back(*name);
            }
        }
        result.certificates_by_website_[std::string(row[0].value().value())].push_back({
            .id_ = std::string(row[1].value().value()),
            .domains_ = std::move(domains),
            .certificate_chain_pem_ = *material->certificate_chain_pem_,
            .private_key_envelope_ = *material->private_key_envelope_,
        });
    }
    co_return result;
}

}  // namespace service::node_dispatch
