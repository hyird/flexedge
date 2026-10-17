#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/db/db_repository.h>
#include <ruvia/web/db/db_transaction.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "service/domains/cache/cache_reference.h"
#include "service/domains/security/security_reference.h"
#include "service/features/certificate_material/model.h"
#include "service/features/node_dispatch/deployment_source.h"
#include "service/features/node_dispatch/dispatch_entity.h"
#include "service/features/website_config/mapper.h"

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
    ruvia::db_query expressions;
    const auto equal = [&](std::string_view column, std::string_view value, std::string_view alias) {
        return expressions.binary(expressions.column(column, alias), ruvia::db_binary_operator::equal,
            expressions.value(value));
    };
    const auto same = [&](std::string_view left_column, std::string_view left_alias,
                          std::string_view right_column, std::string_view right_alias) {
        return expressions.binary(expressions.column(left_column, left_alias), ruvia::db_binary_operator::equal,
            expressions.column(right_column, right_alias));
    };
    auto websites = transaction.get_repository<dispatch_website_entity>().create_query_builder("website");
    websites.select({{"id", {}}, {"revision", {}}, {"enabled", equal("status", "enabled", "website")},
                        {"config", expressions.cast(expressions.column("config", "website"), ruvia::db_data_type::text)}})
        .where(equal("tenant_id", result.tenant_id_, "website"))
        .and_where(equal("cluster_id", result.cluster_id_, "website"))
        .and_where(expressions.unary(ruvia::db_unary_operator::is_null, expressions.column("deleted_at", "website")))
        .order_by(expressions.column("sort", "website"));
    const auto website_rows = co_await websites.get_many<published_website>();
    result.websites_.reserve(website_rows.size());
    for (const auto& website : website_rows) {
        result.websites_.push_back({.id_ = std::string(website.get<"id">()), .revision_ = website.get<"revision">(), .enabled_ = website.get<"enabled">(), .config_json_ = std::string(website.get<"config">())});
        const auto config = service::website_config::parse_stored(website.get<"config">());
        if (config && config->cache_enabled_ && !config->cache_policy_id_.empty()) {
            result.websites_.back().response_cache_ = co_await service::cache::published_policy(transaction, result.tenant_id_, config->cache_policy_id_);
        }
        if (config && !config->security_policy_id_.empty()) {
            result.websites_.back().security_ = co_await service::security::published_policy(transaction, result.tenant_id_, config->security_policy_id_);
        }
    }
    auto certificates = transaction.get_repository<dispatch_binding_entity>().create_query_builder("binding");
    certificates.select({{"website_id", {}}, {"id", expressions.column("id", "certificate")},
                            {"wildcard_domain", expressions.subscript(expressions.column("subject_alt_names", "certificate"), expressions.value(1))},
                            {"base_domain", expressions.subscript(expressions.column("subject_alt_names", "certificate"), expressions.value(2))},
                            {"material", expressions.cast(expressions.column("material", "certificate"), ruvia::db_data_type::text)}})
        .join<dispatch_website_entity>(ruvia::db_join_type::inner, "website",
            expressions.binary(same("tenant_id", "website", "tenant_id", "binding"), ruvia::db_binary_operator::and_value,
                same("id", "website", "website_id", "binding")))
        .join<dispatch_certificate_entity>(ruvia::db_join_type::inner, "certificate",
            expressions.binary(same("tenant_id", "certificate", "tenant_id", "binding"), ruvia::db_binary_operator::and_value,
                same("id", "certificate", "certificate_id", "binding")))
        .where(equal("tenant_id", result.tenant_id_, "binding"))
        .and_where(equal("cluster_id", result.cluster_id_, "website"))
        .and_where(expressions.unary(ruvia::db_unary_operator::is_null, expressions.column("deleted_at", "website")))
        .and_where(expressions.binary(expressions.column("issued_revision", "certificate"), ruvia::db_binary_operator::greater,
            expressions.value(0)))
        .and_where(expressions.binary(expressions.column("expires_at", "certificate"), ruvia::db_binary_operator::greater,
            expressions.call("now")))
        .and_where(expressions.unary(ruvia::db_unary_operator::is_null, expressions.column("deleted_at", "certificate")))
        .order_by(expressions.column("sort", "website"))
        .add_order_by(expressions.column("position", "binding"));
    const auto certificate_rows = co_await certificates.get_many<published_certificate>();
    for (const auto& certificate : certificate_rows) {
        const auto material = service::certificate_material::parse_stored(certificate.get<"material">());
        if (!material || !material->certificate_chain_pem_ || !material->private_key_envelope_) {
            throw std::runtime_error("certificate release material is invalid");
        }
        std::vector<std::string> domains;
        if (!certificate.is_null<"wildcard_domain">() && !certificate.get<"wildcard_domain">().empty()) {
            domains.emplace_back(certificate.get<"wildcard_domain">());
        }
        if (!certificate.is_null<"base_domain">() && !certificate.get<"base_domain">().empty()) {
            domains.emplace_back(certificate.get<"base_domain">());
        }
        result.certificates_by_website_[std::string(certificate.get<"website_id">())].push_back({.id_ = std::string(certificate.get<"id">()), .domains_ = std::move(domains), .certificate_chain_pem_ = *material->certificate_chain_pem_, .private_key_envelope_ = *material->private_key_envelope_});
    }
    co_return result;
}

}  // namespace service::node_dispatch
