#pragma once
#include <ruvia/web/context.h>
#include <ruvia/web/model_json.h>

#include <set>

#include "service/domains/cache/cache_reference.h"
#include "service/domains/cache/cache_types.h"
#include "service/domains/website/website_reference.h"
#include "service/features/node_dispatch/queue.h"
#include "service/features/sync_runtime/state.h"
#include "service/features/sync_runtime/work_commit.h"
namespace service::cache {

class cache_policy_service final {
public:
    static void validate(const policy_input& input) {
        const auto name = input.get<"name">().view();
        if (name.empty() || !flexedge::route_policy::clean_text(name, 100) ||
            name.find_first_not_of(" \t") == std::string_view::npos ||
            !flexedge::route_policy::clean_text(input.get<"description">().view(), 1000) ||
            (input.get<"status">().view() != "enabled" && input.get<"status">().view() != "disabled")) {
            service::common::throw_app_error(19003, "缓存策略名称、描述或状态不正确", 422);
        }
        const auto error = flexedge::cache::validate(service::cache_config::from_dto(input.get<"config">()));
        if (!error.empty()) {
            service::common::throw_app_error(19003, error, 422);
        }
    }

    ruvia::task<policy_dto> detail(auto& c, const std::string& tenant, const std::string& id) const {
        auto db = c.db();
        const auto stored = co_await find_policy(db, tenant, id);
        if (!stored) {
            service::common::throw_app_error(19002, "缓存策略不存在", 404);
        }
        co_return co_await map(c, tenant, *stored);
    }

    ruvia::task<policy_page> list(auto& c, const std::string& tenant, std::int64_t page,
        std::int64_t size, std::int64_t skip, const std::string& keyword, const std::string& status) const {
        ruvia::db_query e(c.pool());
        auto db = c.db();
        auto query = db.template get_repository<policy_entity>().create_query_builder("policy");
        query.where(e.sql({"policy.tenant_id = ", " AND policy.deleted_at IS NULL"}, {e.value(tenant)}));
        if (!keyword.empty()) {
            query.and_where(e.sql({"strpos(lower(policy.name),lower(", ")) > 0"}, {e.value(keyword)}));
        }
        if (!status.empty()) {
            query.and_where(e.sql({"policy.status = ", ""}, {e.value(status)}));
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
        const auto config = ruvia::to_json(service::cache_config::to_dto(service::cache_config::from_dto(input.get<"config">())));
        const auto name = std::string(input.get<"name">().view());
        const auto description = std::string(input.get<"description">().view());
        const auto status = std::string(input.get<"status">().view());
        std::string saved_id = id;
        try {
            auto tx = co_await c.db().begin_transaction();
            co_await service::website::lock_policy_bindings(tx, tenant);
            if (id.empty()) {
                const auto rows = co_await tx.query("INSERT INTO sys_cache_policy(tenant_id,name,description,status,config) VALUES($1,$2,$3,$4,$5::jsonb) RETURNING id", tenant, name, description, status, config);
                saved_id = std::string(rows.front()[0].value().value());
            } else {
                const auto previous = co_await find_policy(tx, tenant, id);
                if (!previous) {
                    service::common::throw_app_error(19002, "缓存策略不存在", 404);
                }
                if (previous->template get<"revision">() != expected) {
                    service::common::throw_app_error(19004, "缓存策略已被修改，请重新加载", 412);
                }
                if (previous->template get<"is_builtin">() && previous->template get<"name">() != std::string_view(name)) {
                    service::common::throw_app_error(19014, "系统内置缓存策略不允许改名", 409);
                }
                const auto previous_config = ruvia::to_json(service::cache_config::to_dto(service::cache_config::parse(previous->template get<"config">()), {}));
                if (previous->template get<"name">() == std::string_view(name) && previous->template get<"description">() == std::string_view(description) && previous->template get<"status">() == std::string_view(status) && previous_config == config) {
                    co_await tx.commit();
                    co_return co_await detail(c, tenant, id);
                }
                (void)co_await tx.execute("UPDATE sys_cache_policy SET name=$3,description=$4,status=$5,config=$6::jsonb,revision=revision+1,updated_at=NOW() WHERE tenant_id=$1 AND id=$2", tenant, id, name, description, status, config);
            }
            service::background::queued_work work;
            service::live_resource::notifications::resource_notification notice(tenant);
            if (!id.empty()) {
                const auto sites = co_await service::website::revise_cache_policy_websites(tx, tenant, id);
                std::set<std::string> clusters;
                for (const auto& site : sites) {
                    const auto& website = site.id_;
                    clusters.emplace(site.cluster_id_);
                    (void)co_await service::sync_runtime::upsert_marker(tx, tenant, service::sync_runtime::marker_resource_type::website, website, service::sync_runtime::marker_operation::apply, site.revision_, work);
                    notice.changed(service::live_resource::resource::websites, website);
                }
                for (const auto& cluster : clusters) {
                    co_await service::node_dispatch::publish_cluster_release(tx, tenant, cluster, work);
                }
                if (!sites.empty()) {
                    notice.node_config();
                }
            }
            co_await service::sync_runtime::commit_work(tx, c, std::move(notice), work);
        } catch (const ruvia::db_error& error) {
            if (error.sql_state() == "23505") {
                service::common::throw_app_error(19005, "缓存策略名称已存在", 409);
            }
            throw;
        }
        co_return co_await detail(c, tenant, saved_id);
    }

    ruvia::task<void> remove(ruvia::context& c, const std::string& tenant, const std::string& id, std::int64_t expected) const {
        auto tx = co_await c.db().begin_transaction();
        co_await service::website::lock_policy_bindings(tx, tenant);
        const auto previous = co_await find_policy(tx, tenant, id);
        if (!previous) {
            service::common::throw_app_error(19002, "缓存策略不存在", 404);
        }
        if (previous->template get<"revision">() != expected) {
            service::common::throw_app_error(19004, "缓存策略已被修改，请重新加载", 412);
        }
        if (previous->template get<"is_builtin">()) {
            service::common::throw_app_error(19013, "系统内置缓存策略不能删除", 409);
        }
        const auto references = co_await service::website::cache_policy_reference_count(tx, tenant, id);
        if (references > 0) {
            service::common::throw_app_error(19006, "策略仍被网站引用，解除引用后才能删除", 409);
        }
        (void)co_await tx.execute("UPDATE sys_cache_policy SET deleted_at=NOW(),updated_at=NOW(),revision=revision+1 WHERE tenant_id=$1 AND id=$2", tenant, id);
        co_await tx.commit();
    }

private:
    static ruvia::task<policy_dto> map(auto& c, const std::string& tenant, const policy_entity& row) {
        policy_dto result({.resource_ = c.pool()});
        result.set<"id">(row.template get<"id">());
        result.set<"name">(row.template get<"name">());
        result.set<"description">(row.template get<"description">());
        result.set<"status">(row.template get<"status">());
        result.set<"is_builtin">(row.template get<"is_builtin">());
        result.set<"revision">(row.template get<"revision">());
        result.set<"created_at">(row.template get<"created_at">());
        result.set<"updated_at">(row.template get<"updated_at">());
        result.set<"config">(service::cache_config::to_dto(service::cache_config::parse(row.template get<"config">()), {.resource_ = c.pool()}));
        auto db = c.db();
        result.set<"website_count">(co_await service::website::cache_policy_reference_count(db, tenant, std::string(row.template get<"id">())));
        co_return result;
    }
};
}  // namespace service::cache
