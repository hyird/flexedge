#pragma once
#include <charconv>
#include <chrono>
#include <unordered_map>
#include <unordered_set>

#include <asio/ip/address.hpp>
#include <asio/ip/network_v4.hpp>
#include <asio/ip/network_v6.hpp>

#include "node/proto/security_validation.h"
#include "service/common/uuid.h"
#include "service/domains/security/security_entity.h"
#include "service/domains/security/security_publication.h"
#include "service/domains/security/security_types.h"
namespace service::security {
inline ruvia::task<std::optional<list_entity>> find_list(auto& db, const std::string& tenant, const std::string& id) {
    ruvia::db_query e;
    auto query = db.template get_repository<list_entity>().create_query_builder("entry");
    query.where(e.sql({"entry.tenant_id = ", " AND entry.id = ", " AND entry.deleted_at IS NULL"}, {e.value(tenant), e.value(id)}));
    co_return co_await query.template get_one<list_entity>();
}
class security_list_service final {
public:
    static list_dto map(auto& c, const list_entity& row) {
        list_dto result({.resource_ = c.pool()});
        result.template set<"id">(row.template get<"id">());
        result.template set<"name">(row.template get<"name">());
        result.template set<"description">(row.template get<"description">());
        result.template set<"kind">(row.template get<"kind">());
        result.template set<"revision">(row.template get<"revision">());
        auto members = ruvia::from_json<ruvia::array<list_member_dto>>(row.template get<"members">(), {.resource_ = c.pool()});
        if (!members) {
            throw std::runtime_error("invalid stored security list");
        }
        result.template set<"members">(std::move(*members));
        return result;
    }
    ruvia::task<ruvia::array<list_dto>> list(auto& c, const std::string& tenant) const {
        ruvia::db_query e(c.pool());
        auto db = c.db();
        auto query = db.template get_repository<list_entity>().create_query_builder("entry");
        query.where(e.sql({"entry.tenant_id = ", " AND entry.deleted_at IS NULL"}, {e.value(tenant)})).take(100);
        const auto rows = co_await query.template get_many<list_entity>();
        ruvia::array<list_dto> result({.resource_ = c.pool()});
        for (const auto& row : rows) {
            result.push_back(map(c, row));
        }
        co_return result;
    }
    ruvia::task<list_dto> detail(auto& c, const std::string& tenant, const std::string& id) const {
        auto db = c.db();
        const auto row = co_await find_list(db, tenant, id);
        if (!row) {
            service::common::throw_app_error(21102, "清单不存在", 404);
        }
        co_return map(c, *row);
    }
    ruvia::task<list_dto> save(ruvia::context& c, const std::string& tenant, const std::string& id, std::int64_t revision, const list_input& input) const {
        const std::string name(input.get<"name">().view()), description(input.get<"description">().view()), kind(input.get<"kind">().view());
        if (name.empty() || name.size() > 50 || name.front() < 'a' || name.front() > 'z' || name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos || !flexedge::route_policy::clean_text(description, 200) || (kind != "ip" && kind != "asn" && kind != "country" && kind != "ua") || input.get<"members">().size() > 100000) {
            service::common::throw_app_error(21103, "清单信息不正确", 422);
        }
        auto tx = co_await c.db().begin_transaction();
        co_await service::website::lock_policy_bindings(tx, tenant);
        const auto previous = id.empty() ? std::optional<list_entity>{} : co_await find_list(tx, tenant, id);
        if (!id.empty() && !previous) {
            service::common::throw_app_error(21102, "清单不存在", 404);
        }
        if (previous && previous->get<"revision">() != revision) {
            service::common::throw_app_error(21104, "清单已被修改，请重新加载", 412);
        }
        if (previous && (previous->get<"name">() != std::string_view(name) || previous->get<"kind">() != std::string_view(kind))) {
            service::common::throw_app_error(21103, "清单标识和类型不能修改", 422);
        }
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        ruvia::array<list_member_dto> members({.resource_ = c.pool()});
        std::unordered_set<std::string> values, identities;
        std::unordered_map<std::string, const list_member_dto*> existing;
        auto prior = previous ? ruvia::from_json<ruvia::array<list_member_dto>>(previous->get<"members">(), {.resource_ = c.pool()}) : std::optional<ruvia::array<list_member_dto>>{};
        if (previous && !prior) {
            throw std::runtime_error("invalid stored security list");
        }
        if (prior) {
            for (const auto& member : *prior) {
                existing.emplace(member.get<"id">().view(), &member);
            }
        }
        flexedge::node::v2::SecurityPolicy probe;
        probe.set_id("validate");
        probe.set_name("validate");
        probe.set_revision(1);
        auto* checked = probe.add_lists();
        checked->set_name(name);
        checked->set_type(kind == "ua" ? "user_agent" : kind);
        for (const auto& member : input.get<"members">()) {
            const auto value = normalize(kind, member.get<"value">().view());
            if (!service::common::parse_uuid(member.get<"id">().view()) || !identities.insert(std::string(member.get<"id">().view())).second || !values.insert(value).second || !flexedge::route_policy::clean_text(member.get<"note">().view(), 800) || !flexedge::route_policy::clean_text(member.get<"source">().view(), 256) || member.get<"expires_unix_ms">().value_ < 0 || member.get<"expires_unix_ms">().value_ > now + 2592000000LL) {
                service::common::throw_app_error(21103, "清单成员不正确", 422);
            }
            checked->add_entries(value);
            auto stored = ruvia::from_json<list_member_dto>(ruvia::to_json(member), {.resource_ = c.pool()}).value();
            stored.set<"value">(value);
            const auto found = existing.find(std::string(member.get<"id">().view()));
            if (found != existing.end()) {
                if (found->second->get<"value">().view() != value || found->second->get<"expires_unix_ms">().value_ != stored.get<"expires_unix_ms">().value_) {
                    service::common::throw_app_error(21103, "已有成员的值和到期时间不能直接修改", 422);
                }
                stored.set<"added_unix_ms">(found->second->get<"added_unix_ms">().value_);
                stored.set<"source">(found->second->get<"source">().view());
                stored.set<"rule_id">(found->second->get<"rule_id">().view());
            } else {
                stored.set<"added_unix_ms">(now);
                stored.set<"source">("手动添加");
                stored.set<"rule_id">("");
            }
            members.push_back(std::move(stored));
        }
        try {
            flexedge::node::validate_security(probe);
        } catch (const std::invalid_argument& error) {
            service::common::throw_app_error(21103, error.what(), 422);
        }
        const auto json = ruvia::to_json(members);
        std::string saved = id;
        try {
            if (previous) {
                if (previous->get<"description">() == std::string_view(description) && ruvia::to_json(*prior) == json) {
                    co_await tx.commit();
                    co_return co_await detail(c, tenant, id);
                }
                (void)co_await tx.execute("UPDATE sys_security_list SET description=$3,members=$4::jsonb,revision=revision+1,updated_at=NOW() WHERE tenant_id=$1 AND id=$2", tenant, id, description, json);
            } else {
                const auto count = co_await tx.query("SELECT count(*) FROM sys_security_list WHERE tenant_id=$1 AND deleted_at IS NULL", tenant);
                if (count.front()[0].as<std::int64_t>().value() >= 100) {
                    service::common::throw_app_error(21103, "最多创建100个清单", 422);
                }
                const auto rows = co_await tx.query("INSERT INTO sys_security_list(tenant_id,name,description,kind,members) VALUES($1,$2,$3,$4,$5::jsonb) RETURNING id", tenant, name, description, kind, json);
                saved = std::string(rows.front()[0].value().value());
            }
            const auto refs = co_await tx.query("SELECT id FROM sys_security_policy WHERE tenant_id=$1 AND deleted_at IS NULL AND config->'lists' @> jsonb_build_array(jsonb_build_object('name',$2::text))", tenant, name);
            std::vector<std::string> policies;
            for (const auto& row : refs) {
                policies.emplace_back(row[0].value().value());
            }
            co_await publish_policy_changes(tx, c, tenant, policies);
        } catch (const ruvia::db_error& error) {
            if (error.sql_state() == "23505") {
                service::common::throw_app_error(21105, "清单名称已存在", 409);
            }
            throw;
        }
        co_return co_await detail(c, tenant, saved);
    }

private:
    static std::string normalize(const std::string& kind, std::string_view value) {
        if (kind != "ip") {
            return std::string(value);
        }
        std::error_code error;
        const auto slash = value.find('/');
        const auto address = asio::ip::make_address(value.substr(0, slash), error);
        if (error || (address.is_v6() && address.to_v6().scope_id())) {
            service::common::throw_app_error(21103, "IP地址不正确", 422);
        }
        if (slash == std::string_view::npos) {
            return address.to_string();
        }
        const auto suffix = value.substr(slash + 1);
        unsigned prefix{};
        const auto parsed = std::from_chars(suffix.data(), suffix.data() + suffix.size(), prefix);
        const auto maximum = address.is_v4() ? 32u : 128u;
        if (parsed.ec != std::errc{} || parsed.ptr != suffix.data() + suffix.size() || prefix > maximum) {
            service::common::throw_app_error(21103, "CIDR前缀不正确", 422);
        }
        if ((address.is_v4() && asio::ip::network_v4(address.to_v4(), static_cast<unsigned short>(prefix)).network() != address.to_v4()) ||
            (address.is_v6() && asio::ip::network_v6(address.to_v6(), static_cast<unsigned short>(prefix)).network() != address.to_v6())) {
            service::common::throw_app_error(21103, "CIDR请填写网络地址", 422);
        }
        return address.to_string() + (prefix == maximum ? "" : "/" + std::to_string(prefix));
    }
};
}  // namespace service::security
