#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/db/db_repository.h>

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "service/common/uuid.h"
#include "service/features/log_ingest/log_mapper.h"
#include "service/features/log_ingest/notification_relay.h"

namespace service::log_ingest {

// The node lock serializes this lookup with every log writer for the same
// node. Its own READ COMMITTED statement sees the preceding writer's commit.
// Event identity is independent of the corrected timestamp and of the Redis
// entry used for a retry or dead-letter replay. Each entity is a distinct kind.
template <typename entity_type>
ruvia::task<void> exclude_stored_events(ruvia::db_transaction& transaction,
    std::string_view tenant, std::string_view node, std::vector<entity_type>& rows) {
    if (rows.empty()) {
        co_return;
    }
    for (auto& row : rows) {
        const auto id = service::common::parse_uuid(std::string_view(row.template get<"id">()));
        if (!id) {
            throw invalid_delivery("invalid log event identity");
        }
        row.template set<"id">(*id);
    }
    std::unordered_set<std::string> seen;
    seen.reserve(rows.size());
    std::vector<std::string> ids;
    ids.reserve(rows.size());
    for (const auto& row : rows) {
        if (seen.emplace(row.template get<"id">()).second) {
            ids.emplace_back(row.template get<"id">());
        }
    }
    const auto matches = entity_type::template column<"tenant_id">() == tenant &&
                         entity_type::template column<"node_id">() == node &&
                         entity_type::template column<"id">().in(std::span<const std::string>(ids));
    ruvia::db_query expressions;
    auto query = transaction.template get_repository<entity_type>().create_query_builder();
    query.select({{"id", {}}}).where(matches.expression(expressions));
    const auto stored = co_await query.template get_many<stored_log_identity>();
    seen.clear();
    for (const auto& row : stored) {
        seen.emplace(row.template get<"id">());
    }
    std::vector<entity_type> fresh;
    fresh.reserve(rows.size());
    for (auto& row : rows) {
        if (seen.emplace(row.template get<"id">()).second) {
            fresh.push_back(std::move(row));
        }
    }
    rows.swap(fresh);
}

// Ownership checks, idempotent bulk inserts and notification outbox rows share
// one transaction. Redis confirmation remains the claiming Worker's next step.
template <typename context_type>
ruvia::task<void> insert_envelope(context_type& context,
    const v2::QueuedLogDelivery& envelope, std::int64_t received_unix_ms) {
    auto rows = to_log_rows(envelope, received_unix_ms);
    const std::string_view tenant{envelope.tenant_id()};
    const std::string_view node{envelope.delivery().node_id()};
    const std::string_view agent{envelope.agent_id()};
    auto transaction = co_await context.db().begin_transaction(
        {.isolation_ = ruvia::db_transaction_isolation::read_committed});
    const auto owner = co_await transaction.template get_repository<log_node_entity>().find_one(
        {.where_ = log_node_entity::column<"tenant_id">() == tenant &&
                   log_node_entity::column<"id">() == node &&
                   log_node_entity::column<"agent_id">() == agent &&
                   log_node_entity::column<"registration_status">() == "registered" &&
                   log_node_entity::column<"deleted_at">().is_null(),
            .lock_ = ruvia::db_lock_options{.mode_ = ruvia::db_row_lock::no_key_update}});
    if (!owner) {
        throw invalid_delivery("log delivery node is no longer registered in tenant scope");
    }
    if (!rows.websites_.empty()) {
        const auto websites = co_await transaction.template get_repository<log_website_entity>().find(
            {.where_ = log_website_entity::column<"tenant_id">() == tenant &&
                       log_website_entity::column<"cluster_id">() == std::string_view(owner->template get<"cluster_id">()) &&
                       log_website_entity::column<"deleted_at">().is_null() &&
                       log_website_entity::column<"id">().in(std::span<const std::string>(rows.websites_)),
                .lock_ = ruvia::db_lock_options{.mode_ = ruvia::db_row_lock::share}});
        if (websites.size() != rows.websites_.size()) {
            throw invalid_delivery("log delivery references a website outside the node cluster");
        }
    }
    co_await exclude_stored_events(transaction, tenant, node, rows.access_);
    co_await exclude_stored_events(transaction, tenant, node, rows.node_);
    const ruvia::db_upsert_options duplicate{.do_nothing_ = true};
    std::vector<std::string> inserted_websites;
    if (!rows.access_.empty()) {
        const auto inserted = co_await transaction.template get_repository<access_log_entity>()
                                  .template upsert_returning<inserted_access_log>(std::span<const access_log_entity>(rows.access_), duplicate);
        for (const auto& row : inserted) {
            inserted_websites.emplace_back(row.template get<"website_id">());
        }
        std::sort(inserted_websites.begin(), inserted_websites.end());
        inserted_websites.erase(std::unique(inserted_websites.begin(), inserted_websites.end()), inserted_websites.end());
    }
    bool node_inserted{};
    if (!rows.node_.empty()) {
        const auto inserted = co_await transaction.template get_repository<node_log_entity>()
                                  .upsert(std::span<const node_log_entity>(rows.node_), duplicate);
        node_inserted = inserted.affected_rows() > 0;
    }
    for (const auto& website : inserted_websites) {
        co_await enqueue_log(transaction, "access", tenant, website);
    }
    if (node_inserted) {
        co_await enqueue_log(transaction, "node", tenant, node);
    }
    if (!inserted_websites.empty() || (node_inserted && std::ranges::any_of(rows.node_, [](const auto& row) {
            return row.template get<"category">() == "security";
        }))) {
        service::live_resource::notifications::resource_notification notice(tenant);
        notice.changed(service::live_resource::resource::security_history);
        (void)co_await service::live_resource::enqueue_notification(transaction, notice);
    }
    co_await transaction.commit();
    service::live_resource::wake_own_outbox(context);
}

}  // namespace service::log_ingest
