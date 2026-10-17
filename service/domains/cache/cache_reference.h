#pragma once
#include <ruvia/core/task.h>
#include <ruvia/web/db/db_repository.h>

#include "service/common/http.h"
#include "service/domains/cache/cache_entity.h"
#include "service/features/cache_config/mapper.h"
namespace service::cache {
// Policy mutation and website binding use the same transaction lock before
// aggregate locks, so a concurrent delete or republish cannot miss a binding.
inline ruvia::task<void> lock_bindings(ruvia::db_transaction& db, const std::string& tenant) {
    co_await db.query("SELECT pg_advisory_xact_lock(hashtextextended($1,0))", "cache-bindings:" + tenant);
}
inline ruvia::task<std::optional<policy_entity>> find_policy(auto& db, const std::string& tenant, const std::string& id) {
    ruvia::db_query e;
    auto query = db.template get_repository<policy_entity>().create_query_builder("policy");
    query.where(e.sql({"policy.tenant_id = ", " AND policy.id = ", " AND policy.deleted_at IS NULL"}, {e.value(tenant), e.value(id)}));
    co_return co_await query.template get_one<policy_entity>();
}
inline ruvia::task<void> validate_website_reference(ruvia::db_transaction& db, const std::string& tenant,
    bool enabled, const std::string& id) {
    if (id.empty()) {
        if (enabled) {
            service::common::throw_app_error(19001, "请选择缓存策略", 422);
        }
        co_return;
    }
    if (!service::common::parse_uuid(id)) {
        service::common::throw_app_error(19002, "缓存策略不存在", 422);
    }
    const auto policy = co_await find_policy(db, tenant, id);
    if (!policy) {
        service::common::throw_app_error(19002, "缓存策略不存在", 422);
    }
}
inline ruvia::task<std::optional<flexedge::cache::policy>> published_policy(auto& db, const std::string& tenant, const std::string& id) {
    const auto row = co_await find_policy(db, tenant, id);
    if (!row) {
        co_return std::nullopt;
    }
    auto value = service::cache_config::parse(row->template get<"config">());
    value.id_ = std::string(row->template get<"id">());
    value.revision_ = row->template get<"revision">();
    value.enabled_ = row->template get<"status">() == "enabled";
    co_return value;
}
}  // namespace service::cache
