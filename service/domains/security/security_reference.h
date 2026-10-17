#pragma once
#include <ruvia/web/db/db_repository.h>
#include <ruvia/web/db/db_transaction.h>

#include "node/proto/edge_control.pb.h"
#include "service/common/http.h"
#include "service/domains/security/security_entity.h"

namespace service::security {
inline ruvia::task<std::optional<policy_entity>> find_policy(auto& db, const std::string& tenant, const std::string& id) {
    ruvia::db_query e;
    auto query = db.template get_repository<policy_entity>().create_query_builder("policy");
    query.where(e.sql({"policy.tenant_id = ", " AND policy.id = ", " AND policy.deleted_at IS NULL"}, {e.value(tenant), e.value(id)}));
    co_return co_await query.template get_one<policy_entity>();
}
ruvia::task<void> validate_website_reference(ruvia::db_transaction& db, const std::string& tenant, const std::string& id);
ruvia::task<flexedge::node::v2::SecurityPolicy> published_policy(ruvia::db_transaction& db, const std::string& tenant, const std::string& id);
}  // namespace service::security
