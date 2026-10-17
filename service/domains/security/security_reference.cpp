#include "service/domains/security/security_reference.h"

#include "service/domains/security/security_types.h"
#include "service/features/security_config/mapper.h"

namespace service::security {
ruvia::task<void> validate_website_reference(ruvia::db_transaction& db, const std::string& tenant, const std::string& id) {
    if (id.empty()) {
        co_return;
    }
    if (!service::common::parse_uuid(id)) {
        service::common::throw_app_error(21002, "安全规则集不存在", 422);
    }
    const auto policy = co_await find_policy(db, tenant, id);
    if (!policy) {
        service::common::throw_app_error(21002, "安全规则集不存在", 422);
    }
}
ruvia::task<flexedge::node::v2::SecurityPolicy> published_policy(ruvia::db_transaction& db, const std::string& tenant, const std::string& id) {
    const auto row = co_await find_policy(db, tenant, id);
    if (!row) {
        throw std::runtime_error("security policy reference is missing");
    }
    auto result = service::security_config::to_proto(service::security_config::parse(row->get<"config">()),
        std::string(row->get<"id">()), std::string(row->get<"name">()), row->get<"revision">());
    for (auto& reference : *result.mutable_lists()) {
        const auto rows = co_await db.query("SELECT kind,members FROM sys_security_list WHERE tenant_id=$1 AND name=$2 AND deleted_at IS NULL", tenant, reference.name());
        if (rows.empty() || (rows.front()[0].value().value() == "ua" ? "user_agent" : rows.front()[0].value().value()) != reference.type()) {
            throw std::runtime_error("security list reference is missing");
        }
        const auto members = ruvia::from_json<ruvia::array<list_member_dto>>(rows.front()[1].value().value());
        if (!members) {
            throw std::runtime_error("invalid stored security list");
        }
        reference.clear_entries();
        for (const auto& source : *members) {
            auto* member = reference.add_members();
            member->set_value(source.get<"value">().view());
            member->set_expires_unix_ms(source.get<"expires_unix_ms">().value_);
        }
    }
    flexedge::node::validate_security(result);
    co_return result;
}
}  // namespace service::security
