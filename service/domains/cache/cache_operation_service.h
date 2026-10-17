#pragma once
#include <ruvia/web/context.h>
#include <ruvia/web/model_json.h>

#include <set>

#include "common/cache_url.h"
#include "service/domains/cache/cache_reference.h"
#include "service/domains/cache/cache_types.h"
#include "service/domains/node/node_reference.h"
#include "service/domains/website/website_reference.h"
#include "service/features/cache_tasks/transport.h"
#include "service/features/sync_runtime/work_commit.h"
#include "service/features/website_config/mapper.h"
namespace service::cache {
class cache_operation_service final {
public:
    ruvia::task<void> retry(ruvia::context& c, const std::string& tenant, const std::string& id) const {
        auto tx = co_await c.db().begin_transaction();
        const auto updated = co_await tx.query("UPDATE sys_cache_task SET status='queued',failure_count=0,error='',next_attempt_at=NOW(),lease_owner=NULL,lease_until=NULL,updated_at=NOW() WHERE tenant_id=$1 AND id=$2 AND status IN ('failed','retrying') AND (lease_until IS NULL OR lease_until<NOW()) RETURNING id", tenant, id);
        if (updated.empty()) {
            service::common::throw_app_error(19012, "任务不存在或正在执行，无法重试", 409);
        }
        service::background::queued_work work;
        work.changed(id);
        work.claim("cache-tasks");
        co_await service::sync_runtime::commit_work(tx, c, tenant, work);
    }
    ruvia::task<ruvia::array<website_option_dto>> websites(auto& c, const std::string& tenant, const std::string& policy_id) const {
        auto db = c.db();
        const auto websites = co_await service::website::cache_website_references(db, tenant, policy_id);
        ruvia::array<website_option_dto> result({.resource_ = c.pool()});
        for (const auto& site : websites) {
            if (policy_id.empty() && !site.config_.cache_enabled_) {
                continue;
            }
            auto& item = result.emplace_back(ruvia::model_options{.resource_ = c.pool()});
            item.template set<"id">(site.id_);
            item.template set<"name">(site.config_.name_.value_or(""));
            item.template set<"status">(site.status_);
            item.template set<"cache_enabled">(site.config_.cache_enabled_);
            item.template set<"cache_policy_id">(site.config_.cache_policy_id_);
            (void)item.template ensure<"domains">();
            for (const auto& domain : site.config_.domains_) {
                item.template ensure<"domains">().emplace_back(domain.hostname_, ruvia::model_options{.resource_ = c.pool()});
            }
        }
        co_return result;
    }
    ruvia::task<operation_created> create(ruvia::context& c, const std::string& tenant, const operation_input& input) const {
        const auto operation = std::string(input.get<"operation">().view());
        const auto object = std::string(input.get<"object_id">().view());
        const auto scope = std::string(input.get<"scope">().view());
        if (!service::common::parse_uuid(object) ||
            !((operation == "prewarm" && scope == "urls") ||
                (operation == "refresh" && (scope == "urls" || scope == "prefix" || scope == "all")) ||
                (operation == "clean" && (scope == "all" || scope == "expired")))) {
            service::common::throw_app_error(19007, "缓存操作或范围不正确", 422);
        }
        const bool needs_urls = scope == "urls" || scope == "prefix";
        if ((needs_urls && input.get<"urls">().empty()) || (!needs_urls && !input.get<"urls">().empty()) || input.get<"urls">().size() > 100) {
            service::common::throw_app_error(19007, "每次提交1至100个URL；整站和清理操作无需填写URL", 422);
        }
        auto tx = co_await c.db().begin_transaction();
        co_await lock_bindings(tx, tenant);
        flexedge::node::v2::CacheCommand command;
        command.set_operation(operation);
        command.set_scope(scope);
        std::set<std::string> clusters;
        std::set<std::string> domains;
        if (operation == "clean") {
            const auto policy = co_await find_policy(tx, tenant, object);
            if (!policy) {
                service::common::throw_app_error(19002, "缓存策略不存在", 404);
            }
            command.set_policy_id(object);
            const auto sites = co_await service::website::cache_website_references(tx, tenant, object);
            for (const auto& site : sites) {
                command.add_website_ids(site.id_);
            }
        } else {
            const auto site = co_await service::website::lock_cache_website(tx, tenant, object);
            if (!site) {
                service::common::throw_app_error(19008, "网站不存在", 404);
            }
            const auto& config = site->config_;
            if (!config.cache_enabled_ || config.cache_policy_id_.empty()) {
                service::common::throw_app_error(19009, "网站尚未启用缓存策略", 422);
            }
            const auto policy = co_await find_policy(tx, tenant, config.cache_policy_id_);
            if (!policy || (operation == "prewarm" && (policy->get<"status">() != "enabled" || site->status_ != "enabled"))) {
                service::common::throw_app_error(19009, "网站或缓存策略已停用，无法预热", 422);
            }
            command.set_policy_id(config.cache_policy_id_);
            command.add_website_ids(object);
            clusters.emplace(site->cluster_id_);
            for (const auto& domain : config.domains_) {
                domains.emplace(domain.hostname_);
            }
        }
        ruvia::array<ruvia::string> urls({.resource_ = c.pool()});
        std::set<std::string> unique_urls;
        for (const auto& value : input.get<"urls">()) {
            const auto url = flexedge::cache::parse_url(value.view());
            if (!url || !domains.contains(url->hostname_) || (scope == "prefix" && url->target_.find('?') != std::string::npos)) {
                service::common::throw_app_error(19010, "URL必须属于所选网站，目录不能包含查询参数或片段", 422);
            }
            if (unique_urls.insert(url->value_).second) {
                urls.emplace_back(url->value_);
                command.add_urls(url->value_);
            }
        }
        std::set<std::string> nodes;
        if (operation == "clean") {
            clusters.emplace("");
        }
        for (const auto& cluster : clusters) {
            const auto targets = co_await service::node::cache_operation_nodes(tx, tenant, cluster);
            nodes.insert(targets.begin(), targets.end());
        }
        if (nodes.empty()) {
            service::common::throw_app_error(19011, "当前范围没有已注册且启用的节点", 422);
        }
        const auto payload = ruvia::to_json(urls);
        const auto rows = co_await tx.query(
            "INSERT INTO sys_cache_task(tenant_id,website_id,cache_policy_id,operation,scope,payload) "
            "VALUES($1,NULLIF($2,'')::uuid,NULLIF($3,'')::uuid,$4,$5,$6::jsonb) "
            "ON CONFLICT(tenant_id,object_id,operation) DO UPDATE SET generation=sys_cache_task.generation+1,scope=EXCLUDED.scope,"
            "payload=EXCLUDED.payload,status='queued',lease_owner=NULL,lease_until=NULL,next_attempt_at=NOW(),"
            "error='',failure_count=0,updated_at=NOW() RETURNING id,generation",
            tenant, operation == "clean" ? std::string{} : object, operation == "clean" ? object : std::string{}, operation, scope, payload);
        const auto id = std::string(rows.front()[0].value().value());
        const auto generation = rows.front()[1].as<std::int64_t>().value();
        command.set_task_id(id);
        command.set_generation(generation);
        (void)co_await tx.execute("DELETE FROM sys_cache_task_target WHERE tenant_id=$1 AND task_id=$2", tenant, id);
        const auto encoded = service::cache_tasks::encode(command);
        for (const auto& node : nodes) {
            (void)co_await tx.execute("INSERT INTO sys_cache_task_target(tenant_id,task_id,generation,node_id,command_envelope) VALUES($1,$2,$3,$4,$5)", tenant, id, generation, node, encoded);
        }
        service::background::queued_work work;
        work.changed(id);
        work.claim("cache-tasks");
        service::live_resource::notifications::resource_notification notice(tenant);
        notice.changed(service::live_resource::resource::tasks, id);
        co_await service::sync_runtime::commit_work(tx, c, std::move(notice), work);
        operation_created result({.resource_ = c.pool()});
        result.set<"id">(id);
        result.set<"target_node_count">(static_cast<std::int64_t>(nodes.size()));
        co_return result;
    }

    ruvia::task<ruvia::array<operation_dto>> list(auto& c, const std::string& tenant, const std::string& operation) const {
        const auto rows = co_await c.db().query(
            "SELECT task.id,task.operation,task.object_id,COALESCE(website.config->>'name',policy.name,''),task.scope,task.status,task.error,"
            "task.updated_at::text,task.generation FROM sys_cache_task task "
            "LEFT JOIN sys_website website ON website.tenant_id=task.tenant_id AND website.id=task.website_id "
            "LEFT JOIN sys_cache_policy policy ON policy.tenant_id=task.tenant_id AND policy.id=task.cache_policy_id "
            "WHERE task.tenant_id=$1 AND task.operation=$2 ORDER BY task.updated_at DESC LIMIT 20",
            tenant, operation);
        ruvia::array<operation_dto> result({.resource_ = c.pool()});
        for (const auto& row : rows) {
            auto& item = result.emplace_back(ruvia::model_options{.resource_ = c.pool()});
            item.template set<"id">(row[0].value().value());
            item.template set<"operation">(row[1].value().value());
            item.template set<"object_id">(row[2].value().value());
            item.template set<"name">(row[3].value().value());
            (void)item.template ensure<"targets">();
            item.template set<"scope">(row[4].value().value());
            item.template set<"status">(row[5].value().value());
            item.template set<"error">(row[6].value().value());
            item.template set<"updated_at">(row[7].value().value());
            const auto targets = co_await c.db().query("SELECT target.node_id,node.name,target.status,target.error,target.affected_objects,target.affected_bytes,target.completed_urls,target.failures::text FROM sys_cache_task_target target JOIN sys_node node ON node.tenant_id=target.tenant_id AND node.id=target.node_id WHERE target.tenant_id=$1 AND target.task_id=$2 AND target.generation=$3 ORDER BY node.name", tenant, std::string(row[0].value().value()), row[8].template as<std::int64_t>().value());
            for (const auto& target : targets) {
                auto& value = item.template ensure<"targets">().emplace_back(ruvia::model_options{.resource_ = c.pool()});
                value.template set<"node_id">(target[0].value().value());
                value.template set<"node_name">(target[1].value().value());
                value.template set<"status">(target[2].value().value());
                value.template set<"error">(target[3].value().value());
                value.template set<"affected_objects">(target[4].template as<std::int64_t>().value());
                value.template set<"affected_bytes">(target[5].template as<std::int64_t>().value());
                value.template set<"completed_urls">(target[6].template as<std::int64_t>().value());
                const auto failures = ruvia::from_json<ruvia::array<ruvia::string>>(target[7].value().value(), {.resource_ = c.pool()});
                if (failures) {
                    value.template set<"failures">(*failures);
                }
            }
        }
        co_return result;
    }
};
}  // namespace service::cache
