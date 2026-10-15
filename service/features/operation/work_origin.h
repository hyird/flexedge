#pragma once

#include <ruvia/core/task.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "service/features/live_resource/notifications.h"

// Tasks of the task center are operations: one user operation (or one system
// action) and the background work it caused, its subtasks. Every write that
// schedules background work passes a work_origin, so each queued marker, node
// release target and recorded attempt names the subtask it belongs to
// (docs/task-center.md). Operations never name a worker instance; claims,
// leases and retries stay with the markers and targets.
namespace service::operation {

enum class action {
    website_create,
    website_update,
    website_enable,
    website_disable,
    website_delete,
    website_check_domains,
    cluster_create,
    cluster_update,
    cluster_delete,
    node_create,
    node_update,
    node_delete,
    node_reset_credentials,
    dns_zone_create,
    dns_zone_update_records,
    dns_zone_sync,
    dns_zone_sync_local,
    dns_zone_sync_remote,
    dns_zone_delete,
    certificate_create,
    certificate_renew,
    certificate_delete,
    provider_update,
    provider_verify,
    provider_delete,
    system_certificate_auto_renew,
    system_certificate_expired,
    system_node_register,
    system_release_upgrade,
    system_dns_cleanup,
    system_untracked,
};

[[nodiscard]] inline constexpr std::string_view action_code(action value) {
    switch (value) {
        case action::website_create:
            return "website.create";
        case action::website_update:
            return "website.update";
        case action::website_enable:
            return "website.enable";
        case action::website_disable:
            return "website.disable";
        case action::website_delete:
            return "website.delete";
        case action::website_check_domains:
            return "website.check_domains";
        case action::cluster_create:
            return "cluster.create";
        case action::cluster_update:
            return "cluster.update";
        case action::cluster_delete:
            return "cluster.delete";
        case action::node_create:
            return "node.create";
        case action::node_update:
            return "node.update";
        case action::node_delete:
            return "node.delete";
        case action::node_reset_credentials:
            return "node.reset_credentials";
        case action::dns_zone_create:
            return "dns_zone.create";
        case action::dns_zone_update_records:
            return "dns_zone.update_records";
        case action::dns_zone_sync:
            return "dns_zone.sync";
        case action::dns_zone_sync_local:
            return "dns_zone.sync_local";
        case action::dns_zone_sync_remote:
            return "dns_zone.sync_remote";
        case action::dns_zone_delete:
            return "dns_zone.delete";
        case action::certificate_create:
            return "certificate.create";
        case action::certificate_renew:
            return "certificate.renew";
        case action::certificate_delete:
            return "certificate.delete";
        case action::provider_update:
            return "provider.update";
        case action::provider_verify:
            return "provider.verify";
        case action::provider_delete:
            return "provider.delete";
        case action::system_certificate_auto_renew:
            return "system.certificate_auto_renew";
        case action::system_certificate_expired:
            return "system.certificate_expired";
        case action::system_node_register:
            return "system.node_register";
        case action::system_release_upgrade:
            return "system.release_upgrade";
        case action::system_dns_cleanup:
            return "system.dns_cleanup";
        case action::system_untracked:
            return "system.untracked";
    }
    throw std::invalid_argument("unsupported operation action");
}

enum class subtask_kind {
    dns_sync,
    dns_challenge,
    dns_challenge_cleanup,
    dns_delete,
    certificate_issue,
    certificate_renew,
    website_check,
    provider_verify,
    node_publish,
    node_config,
};

[[nodiscard]] inline constexpr std::string_view subtask_kind_name(subtask_kind value) {
    switch (value) {
        case subtask_kind::dns_sync:
            return "dns_sync";
        case subtask_kind::dns_challenge:
            return "dns_challenge";
        case subtask_kind::dns_challenge_cleanup:
            return "dns_challenge_cleanup";
        case subtask_kind::dns_delete:
            return "dns_delete";
        case subtask_kind::certificate_issue:
            return "certificate_issue";
        case subtask_kind::certificate_renew:
            return "certificate_renew";
        case subtask_kind::website_check:
            return "website_check";
        case subtask_kind::provider_verify:
            return "provider_verify";
        case subtask_kind::node_publish:
            return "node_publish";
        case subtask_kind::node_config:
            return "node_config";
    }
    throw std::invalid_argument("unsupported subtask kind");
}

// Settled subtask states; an open subtask's status follows its marker,
// release target or node (see the task read model).
inline constexpr std::string_view state_open{"open"};
inline constexpr std::string_view state_completed{"completed"};
inline constexpr std::string_view state_merged{"merged"};
inline constexpr std::string_view state_cancelled{"cancelled"};

// Scalar SQL naming a resource of `resource_type` ("website", "cluster",
// "node", "dns_zone", "certificate", "provider"), with the tenant and id
// bound at the given placeholders. Deleted resources keep their name.
[[nodiscard]] inline std::string resource_name_sql(std::string_view resource_type, std::string_view tenant,
    std::string_view id) {
    const auto where = std::string(" WHERE tenant_id = ") + std::string(tenant) + " AND id = " + std::string(id);
    if (resource_type == "website") {
        return "(SELECT config->>'name' FROM sys_website" + where + ")";
    }
    if (resource_type == "cluster") {
        return "(SELECT name FROM sys_cluster" + where + ")";
    }
    if (resource_type == "node") {
        return "(SELECT name FROM sys_node" + where + ")";
    }
    if (resource_type == "dns_zone") {
        return "(SELECT domain FROM sys_dns_zone" + where + ")";
    }
    if (resource_type == "certificate") {
        return "(SELECT domain FROM sys_certificate" + where + ")";
    }
    if (resource_type == "provider") {
        return "(SELECT COALESCE(name, provider) FROM sys_provider" + where + ")";
    }
    throw std::invalid_argument("unsupported operation resource type");
}

// Top-level keys whose values differ between two JSON objects (a saved
// configuration before and after a write), in key order.
template <typename database_type>
ruvia::task<std::vector<std::string>> changed_keys(database_type& database, std::string_view previous_json,
    std::string_view next_json) {
    const auto rows = co_await database.query(
        "SELECT COALESCE(next.key, previous.key) FROM jsonb_each($1::jsonb) previous FULL JOIN "
        "jsonb_each($2::jsonb) next ON next.key = previous.key WHERE next.value IS DISTINCT FROM "
        "previous.value ORDER BY 1",
        previous_json, next_json);
    std::vector<std::string> keys;
    keys.reserve(rows.size());
    for (const auto& row : rows) {
        keys.emplace_back(row[0].value().value_or(""));
    }
    co_return keys;
}

// Request summary `{"sections":[...]}` of changed section names (fixed
// identifiers), `{}` without any.
[[nodiscard]] inline std::string sections_summary(const std::vector<std::string_view>& sections) {
    if (sections.empty()) {
        return "{}";
    }
    std::string summary = "{\"sections\":[";
    for (std::size_t index = 0; index < sections.size(); ++index) {
        if (index != 0) {
            summary += ',';
        }
        summary += '"';
        summary += sections[index];
        summary += '"';
    }
    summary += "]}";
    return summary;
}

// Adds `section` once, keeping first-seen order.
inline void add_section(std::vector<std::string_view>& sections, std::string_view section) {
    if (std::find(sections.begin(), sections.end(), section) == sections.end()) {
        sections.push_back(section);
    }
}

// The subtask doing a piece of background work, and its task. Empty for
// work that belongs to no task (scheduled before the upgrade, or by a
// server without task tracking).
struct work_owner final {
    std::string operation_id_;
    std::string subtask_id_;

    [[nodiscard]] bool empty() const noexcept {
        return subtask_id_.empty();
    }
};

// What a subtask does on its resource besides the marker operation: the
// DNS-01 records of a certificate issuance are published and retired with
// ordinary DNS syncs.
enum class subtask_purpose {
    work,
    dns_challenge,
    dns_challenge_cleanup,
};

struct operation_request final {
    action action_;
    // Administrator who started it; empty for a system operation.
    std::string actor_id_;
    // "website", "cluster", "node", "dns_zone", "certificate" or "provider".
    std::string target_type_;
    std::string target_id_;
    // Request summary (docs/task-center.md): section names or counts only.
    std::string summary_{"{}"};
};

// Settles an open subtask. `merged_into`: the subtask that took over its
// work; subtasks merged into this one follow it. Returns the subtask's task
// when it was open, otherwise nothing.
template <typename database_type>
ruvia::task<std::optional<std::string>> settle_subtask(database_type& database, std::string_view tenant_id,
    std::string_view subtask_id, std::string_view state, std::string_view merged_into = {},
    std::string_view closed_by = {}) {
    if (subtask_id.empty() || subtask_id == merged_into) {
        co_return std::nullopt;
    }
    const auto rows = co_await database.query(
        "UPDATE sys_operation_subtask SET state = $3, merged_into_id = NULLIF($4, '')::uuid, "
        "closed_by_operation_id = NULLIF($5, '')::uuid, closed_at = NOW() WHERE tenant_id = $1 AND "
        "id = $2 AND state = 'open' RETURNING operation_id::text",
        tenant_id, subtask_id, state, merged_into, closed_by);
    if (rows.empty()) {
        co_return std::nullopt;
    }
    if (!merged_into.empty()) {
        (void)co_await database.execute(
            "UPDATE sys_operation_subtask SET merged_into_id = $3::uuid WHERE tenant_id = $1 AND "
            "merged_into_id = $2::uuid AND id <> $3::uuid",
            tenant_id, subtask_id, merged_into);
    }
    co_return std::string(rows.front()[0].value().value_or(""));
}

// Where a write's background work comes from. Passed by reference through
// every enqueue and publish path of one transaction; it creates the user or
// system task on its first subtask and remembers every task whose state the
// transaction changed, which the caller notifies after the commit.
class work_origin final {
public:
    enum class mode {
        // A new task: a user operation or a system action.
        start,
        // Work a running subtask causes: subtasks of its task under it.
        caused_by,
        // The running subtask continues on a newer version of its resource.
        continuing,
        // Maintenance re-creating a lost marker: the subtask of that version
        // keeps (or regains) the work; nothing new is created.
        repair,
    };

    [[nodiscard]] static work_origin start(operation_request request) {
        work_origin origin(mode::start);
        origin.request_ = std::move(request);
        return origin;
    }
    [[nodiscard]] static work_origin system(action value, std::string target_type, std::string target_id) {
        return start({.action_ = value,
            .actor_id_ = {},
            .target_type_ = std::move(target_type),
            .target_id_ = std::move(target_id),
            .summary_ = "{}"});
    }
    [[nodiscard]] static work_origin caused_by(work_owner parent,
        subtask_purpose purpose = subtask_purpose::work) {
        work_origin origin(mode::caused_by);
        origin.owner_ = std::move(parent);
        origin.purpose_ = purpose;
        return origin;
    }
    [[nodiscard]] static work_origin continuing(work_owner owner) {
        work_origin origin(mode::continuing);
        origin.owner_ = std::move(owner);
        return origin;
    }
    [[nodiscard]] static work_origin repair() {
        return work_origin(mode::repair);
    }

    [[nodiscard]] mode get_mode() const noexcept {
        return mode_;
    }
    [[nodiscard]] subtask_purpose purpose() const noexcept {
        return purpose_;
    }
    [[nodiscard]] const work_owner& owner() const noexcept {
        return owner_;
    }

    // The task new subtasks join, created by its first subtask (start), or
    // nothing when this origin creates no subtasks.
    [[nodiscard]] std::optional<std::string> operation_id() const {
        if (mode_ == mode::start) {
            return created_;
        }
        if ((mode_ == mode::caused_by || mode_ == mode::continuing) && !owner_.empty()) {
            return owner_.operation_id_;
        }
        return std::nullopt;
    }

    template <typename database_type>
    ruvia::task<std::optional<std::string>> require_operation(database_type& database, std::string_view tenant_id) {
        if (mode_ != mode::start || created_) {
            co_return operation_id();
        }
        const auto rows = co_await database.query(
            "INSERT INTO sys_operation (tenant_id, action, actor_type, actor_id, actor_name, "
            "target_type, target_id, target_name, summary) SELECT $1::uuid, $2, CASE WHEN $3 = '' THEN "
            "'system' ELSE 'user' END, NULLIF($3, '')::uuid, COALESCE((SELECT "
            "COALESCE(NULLIF(admin.nickname, ''), admin.username) FROM sys_admin admin WHERE "
            "admin.id = NULLIF($3, '')::uuid), ''), $4, $5::uuid, left(COALESCE(" +
                resource_name_sql(request_.target_type_, "$1", "$5::uuid") +
                ", ''), 253), $6::jsonb RETURNING id::text",
            tenant_id, action_code(request_.action_), std::string_view(request_.actor_id_),
            std::string_view(request_.target_type_), std::string_view(request_.target_id_),
            std::string_view(request_.summary_));
        if (rows.empty()) {
            throw std::runtime_error("task could not be created");
        }
        created_ = std::string(rows.front()[0].value().value_or(""));
        touch(*created_);
        co_return created_;
    }

    // The subtask of this origin doing `kind` work on a resource at
    // `version`, or nothing for unowned work and repairs. One subtask per
    // (task, parent, kind, resource): repeated work (a retried issuance
    // publishing its challenge again, one write dirtying a zone twice) reuses
    // and reopens it.
    template <typename database_type>
    ruvia::task<std::optional<std::string>> require_subtask(database_type& database, std::string_view tenant_id,
        subtask_kind kind, std::string_view resource_type, std::string_view resource_id, std::int64_t version) {
        if (mode_ == mode::repair || ((mode_ == mode::caused_by || mode_ == mode::continuing) && owner_.empty())) {
            co_return std::nullopt;
        }
        if (mode_ == mode::continuing) {
            co_await reopen(database, tenant_id, owner_.subtask_id_, version);
            touch(owner_.operation_id_);
            co_return owner_.subtask_id_;
        }
        const auto operation = co_await require_operation(database, tenant_id);
        const std::string_view parent = mode_ == mode::caused_by ? std::string_view(owner_.subtask_id_)
                                                                 : std::string_view{};
        const auto existing = co_await database.query(
            "SELECT id::text FROM sys_operation_subtask WHERE tenant_id = $1 AND operation_id = $2::uuid "
            "AND parent_id IS NOT DISTINCT FROM NULLIF($3, '')::uuid AND kind = $4 AND resource_id = "
            "$5::uuid ORDER BY created_at DESC LIMIT 1 FOR UPDATE",
            tenant_id, std::string_view(*operation), parent, subtask_kind_name(kind), resource_id);
        if (!existing.empty()) {
            const auto id = std::string(existing.front()[0].value().value_or(""));
            co_await reopen(database, tenant_id, id, version);
            touch(*operation);
            co_return id;
        }
        const auto rows = co_await database.query(
            "INSERT INTO sys_operation_subtask (tenant_id, operation_id, parent_id, kind, "
            "resource_type, resource_id, resource_name, version) VALUES ($1, $2::uuid, NULLIF($3, "
            "'')::uuid, $4, $5, $6::uuid, left(COALESCE(" +
                resource_name_sql(resource_type, "$1", "$6::uuid") + ", ''), 253), $7) RETURNING id::text",
            tenant_id, std::string_view(*operation), parent, subtask_kind_name(kind), resource_type, resource_id,
            version);
        if (rows.empty()) {
            throw std::runtime_error("subtask could not be created");
        }
        touch(*operation);
        co_return std::string(rows.front()[0].value().value_or(""));
    }

    // A task whose state this transaction changed.
    void touch(std::string_view operation_id) {
        if (!operation_id.empty() &&
            std::find(touched_.begin(), touched_.end(), operation_id) == touched_.end()) {
            touched_.emplace_back(operation_id);
        }
    }
    [[nodiscard]] const std::vector<std::string>& touched() const noexcept {
        return touched_;
    }
    // Adds the changed tasks to the notification sent after the commit.
    void add_changes(service::live_resource::notifications::resource_notification& notification) const {
        for (const auto& id : touched_) {
            notification.changed(service::live_resource::resource::operations, id);
        }
    }

private:
    explicit work_origin(mode value)
        : mode_(value) {}

    template <typename database_type>
    static ruvia::task<void> reopen(database_type& database, std::string_view tenant_id, std::string_view id,
        std::int64_t version) {
        (void)co_await database.execute(
            "UPDATE sys_operation_subtask SET version = GREATEST(version, $3), state = 'open', "
            "merged_into_id = NULL, closed_by_operation_id = NULL, closed_at = NULL WHERE tenant_id = $1 "
            "AND id = $2::uuid",
            tenant_id, id, version);
    }

    mode mode_;
    subtask_purpose purpose_{subtask_purpose::work};
    operation_request request_{};
    work_owner owner_{};
    std::optional<std::string> created_;
    std::vector<std::string> touched_;
};

// Reopens the newest subtask that did `resource`'s work at `version` (a
// repaired marker), returning its owner; nothing when none is retained.
template <typename database_type>
ruvia::task<work_owner> reopen_subtask_of_version(database_type& database, std::string_view tenant_id,
    std::string_view resource_type, std::string_view resource_id, std::int64_t version) {
    const auto rows = co_await database.query(
        "UPDATE sys_operation_subtask SET state = 'open', merged_into_id = NULL, "
        "closed_by_operation_id = NULL, closed_at = NULL WHERE id = (SELECT id FROM "
        "sys_operation_subtask WHERE tenant_id = $1 AND resource_type = $2 AND resource_id = $3::uuid "
        "AND version = $4 AND kind NOT IN ('node_publish', 'node_config') ORDER BY created_at DESC "
        "LIMIT 1) RETURNING operation_id::text, id::text",
        tenant_id, resource_type, resource_id, version);
    if (rows.empty()) {
        co_return work_owner{};
    }
    co_return work_owner{.operation_id_ = std::string(rows.front()[0].value().value_or("")),
        .subtask_id_ = std::string(rows.front()[1].value().value_or(""))};
}

}  // namespace service::operation
