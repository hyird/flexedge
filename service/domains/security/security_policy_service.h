#pragma once
#include <ruvia/web/context.h>

#include <array>

#include "service/domains/security/security_publication.h"
#include "service/domains/security/security_reference.h"
#include "service/domains/security/security_types.h"
#include "service/domains/website/website_reference.h"
#include "service/features/node_dispatch/queue.h"
#include "service/features/security_config/mapper.h"
#include "service/features/sync_runtime/state.h"
#include "service/features/sync_runtime/work_commit.h"

namespace service::security {
class security_policy_service final {
public:
    ruvia::task<ruvia::array<policy_option_dto>> options(ruvia::context& c, const std::string& tenant) const {
        auto db = c.db();
        const auto rows = co_await db.query("SELECT policy.id,policy.name,policy.description,(SELECT count(*) FROM sys_website site WHERE site.tenant_id=policy.tenant_id AND site.security_policy_id=policy.id AND site.deleted_at IS NULL) FROM sys_security_policy policy WHERE policy.tenant_id=$1 AND policy.deleted_at IS NULL ORDER BY policy.sort DESC LIMIT 1000", tenant);
        ruvia::array<policy_option_dto> result({.resource_ = c.pool()});
        for (const auto& row : rows) {
            policy_option_dto value({.resource_ = c.pool()});
            value.set<"id">(row[0].value().value());
            value.set<"name">(row[1].value().value());
            value.set<"description">(row[2].value().value());
            value.set<"website_count">(row[3].as<std::int64_t>().value());
            result.push_back(std::move(value));
        }
        co_return result;
    }
    static void validate(const policy_input& input) {
        const auto name = input.get<"name">().view();
        if (name.empty() || !flexedge::route_policy::clean_text(name, 256) ||
            name.find_first_not_of(" \t") == std::string_view::npos ||
            !flexedge::route_policy::clean_text(input.get<"description">().view(), 1000)) {
            service::common::throw_app_error(21003, "安全规则集名称或描述不正确", 422);
        }
        try {
            (void)service::security_config::to_proto(input.get<"config">(), "validate", std::string(name), 1);
        } catch (const std::invalid_argument& error) {
            service::common::throw_app_error(21003, error.what(), 422);
        }
    }

    ruvia::task<policy_dto> detail(auto& c, const std::string& tenant, const std::string& id) const {
        auto db = c.db();
        const auto row = co_await find_policy(db, tenant, id);
        if (!row) {
            service::common::throw_app_error(21002, "安全规则集不存在", 404);
        }
        co_return co_await map(c, tenant, *row);
    }

    ruvia::task<policy_page> list(auto& c, const std::string& tenant, std::int64_t page,
        std::int64_t size, std::int64_t skip, const std::string& keyword) const {
        ruvia::db_query e(c.pool());
        auto db = c.db();
        auto query = db.template get_repository<policy_entity>().create_query_builder("policy");
        query.where(e.sql({"policy.tenant_id = ", " AND policy.deleted_at IS NULL"}, {e.value(tenant)}));
        if (!keyword.empty()) {
            query.and_where(e.sql({"strpos(lower(policy.name),lower(", ")) > 0"}, {e.value(keyword)}));
        }
        const auto total = co_await query.get_count();
        query.order_by(e.column("sort", "policy"), ruvia::db_order_direction::desc).take(static_cast<std::uint64_t>(size)).skip(static_cast<std::uint64_t>(skip));
        const auto rows = co_await query.template get_many<policy_entity>();
        policy_page result({.resource_ = c.pool()});
        (void)result.ensure<"list">();
        result.set<"total">(total);
        result.set<"page">(page);
        result.set<"page_size">(size);
        result.set<"total_pages">((total + size - 1) / size);
        for (const auto& row : rows) {
            result.ensure<"list">().push_back(co_await map(c, tenant, row));
        }
        co_return result;
    }

    ruvia::task<policy_dto> save(ruvia::context& c, const std::string& tenant, const std::string& id,
        std::int64_t expected, const policy_input& input) const {
        validate(input);
        const auto config = ruvia::to_json(input.get<"config">());
        const auto name = std::string(input.get<"name">().view());
        const auto description = std::string(input.get<"description">().view());
        std::string saved = id;
        try {
            auto tx = co_await c.db().begin_transaction();
            co_await service::website::lock_policy_bindings(tx, tenant);
            for (const auto& reference : input.get<"config">().get<"lists">()) {
                const auto rows = co_await tx.query("SELECT kind FROM sys_security_list WHERE tenant_id=$1 AND name=$2 AND deleted_at IS NULL", tenant, reference.get<"name">().view());
                const auto type = reference.get<"type">().view();
                if (rows.empty() || (rows.front()[0].value().value() == "ua" ? "user_agent" : rows.front()[0].value().value()) != type) {
                    service::common::throw_app_error(21003, "共享清单不存在或类型不匹配", 422);
                }
                if (!reference.get<"entries">().empty()) {
                    service::common::throw_app_error(21003, "请在清单详情中维护共享成员", 422);
                }
            }
            if (id.empty()) {
                const auto rows = co_await tx.query("INSERT INTO sys_security_policy(tenant_id,name,description,config) VALUES($1,$2,$3,$4::jsonb) RETURNING id", tenant, name, description, config);
                saved = std::string(rows.front()[0].value().value());
            } else {
                const auto previous = co_await find_policy(tx, tenant, id);
                require_revision(previous, expected);
                const auto previous_config = ruvia::to_json(service::security_config::parse(previous->template get<"config">()));
                if (previous->template get<"name">() == std::string_view(name) &&
                    previous->template get<"description">() == std::string_view(description) && previous_config == config) {
                    co_await tx.commit();
                    co_return co_await detail(c, tenant, id);
                }
                (void)co_await tx.execute("UPDATE sys_security_policy SET name=$3,description=$4,config=$5::jsonb,revision=revision+1,updated_at=NOW() WHERE tenant_id=$1 AND id=$2", tenant, id, name, description, config);
            }
            const std::array<std::string, 1> affected{saved};
            co_await publish_policy_changes(tx, c, tenant, affected);
        } catch (const ruvia::db_error& error) {
            if (error.sql_state() == "23505") {
                service::common::throw_app_error(21005, "安全规则集名称已存在", 409);
            }
            throw;
        }
        co_return co_await detail(c, tenant, saved);
    }

    ruvia::task<void> remove(ruvia::context& c, const std::string& tenant, const std::string& id, std::int64_t expected) const {
        auto tx = co_await c.db().begin_transaction();
        co_await service::website::lock_policy_bindings(tx, tenant);
        require_revision(co_await find_policy(tx, tenant, id), expected);
        if (co_await service::website::security_policy_reference_count(tx, tenant, id)) {
            service::common::throw_app_error(21006, "规则集仍被网站引用", 409);
        }
        (void)co_await tx.execute("UPDATE sys_security_policy SET deleted_at=NOW(),updated_at=NOW(),revision=revision+1 WHERE tenant_id=$1 AND id=$2", tenant, id);
        co_await tx.commit();
    }

private:
    static void require_revision(const std::optional<policy_entity>& row, std::int64_t expected) {
        if (!row) {
            service::common::throw_app_error(21002, "安全规则集不存在", 404);
        }
        if (row->get<"revision">() != expected) {
            service::common::throw_app_error(21004, "规则集已被修改，请重新加载", 412);
        }
    }
    static ruvia::task<policy_dto> map(auto& c, const std::string& tenant, const policy_entity& row) {
        policy_dto result({.resource_ = c.pool()});
        result.set<"id">(row.get<"id">());
        result.set<"name">(row.get<"name">());
        result.set<"description">(row.get<"description">());
        result.set<"revision">(row.get<"revision">());
        result.set<"created_at">(row.get<"created_at">());
        result.set<"updated_at">(row.get<"updated_at">());
        result.set<"config">(service::security_config::parse(row.get<"config">(), {.resource_ = c.pool()}));
        auto db = c.db();
        result.set<"website_count">(co_await service::website::security_policy_reference_count(db, tenant, std::string(row.get<"id">())));
        co_return result;
    }
};
}  // namespace service::security
