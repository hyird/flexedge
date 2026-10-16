#pragma once

#include <ruvia/core/timer.h>
#include <ruvia/web/db/db_repository.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "service/features/background/work_available.h"
#include "service/features/live_resource/notification_outbox.h"
#include "service/features/logging/logger.h"
#include "service/features/node_channel/projection.h"
#include "service/features/node_channel/report_mapper.h"
#include "service/features/node_channel/report_queue.h"
#include "service/features/node_channel/report_receipt_entity.h"
#include "service/features/node_channel/report_service.h"
#include "service/utils/token.h"

namespace service::node_channel {

class invalid_report final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

inline std::string received_timestamp(std::int64_t milliseconds) {
    const std::chrono::sys_time<std::chrono::milliseconds> point{std::chrono::milliseconds(milliseconds)};
    const auto day = std::chrono::floor<std::chrono::days>(point);
    const std::chrono::year_month_day date{day};
    const std::chrono::hh_mm_ss clock{point - day};
    std::array<char, 40> text{};
    const auto length = std::snprintf(text.data(), text.size(), "%04d-%02u-%02uT%02lld:%02lld:%02lld.%03lldZ",
        static_cast<int>(date.year()), static_cast<unsigned>(date.month()), static_cast<unsigned>(date.day()),
        static_cast<long long>(clock.hours().count()), static_cast<long long>(clock.minutes().count()),
        static_cast<long long>(clock.seconds().count()), static_cast<long long>(clock.subseconds().count()));
    if (length <= 0 || static_cast<std::size_t>(length) >= text.size()) {
        throw invalid_report("invalid_report_timestamp");
    }
    return {text.data(), static_cast<std::size_t>(length)};
}

inline ruvia::task<std::optional<report_node_entity>> lock_report_node(
    ruvia::db_transaction& transaction, std::string_view tenant, std::string_view node) {
    using entity = report_node_entity;
    co_return co_await transaction.get_repository<entity>().find_one({.where_ = entity::column<"tenant_id">() == tenant && entity::column<"id">() == node,
        .lock_ = ruvia::db_lock_options{.mode_ = ruvia::db_row_lock::update}});
}

// The node row lock precedes the ownership check and the durable receipt.
// Cleanup takes that same lock before checking source absence and deleting the
// receipt. A delayed former owner therefore cannot run after ledger cleanup.
template <typename context_type>
ruvia::task<bool> persist_report(context_type& context, std::string_view consumer,
    const report_queue::entry& entry, const report& value) {
    auto transaction = co_await context.db().begin_transaction();
    const auto node = co_await lock_report_node(transaction, value.principal_.tenant_id_, value.principal_.node_id_);
    if (!(co_await report_queue::owns(context.redis(), consumer, entry))) {
        co_return false;
    }
    using receipt_entity = report_receipt_entity;
    const auto previous = co_await transaction.template get_repository<receipt_entity>().find_one(
        {.where_ = receipt_entity::column<"id">() == std::string_view(entry.report_id_)});
    if (previous) {
        if (std::string_view(previous->template get<"tenant_id">()) != value.principal_.tenant_id_ ||
            std::string_view(previous->template get<"node_id">()) != value.principal_.node_id_ ||
            std::string_view(previous->template get<"agent_id">()) != value.principal_.agent_id_) {
            throw invalid_report("report_receipt_identity_mismatch");
        }
        co_await transaction.commit();
        co_return true;
    }
    if (!node || !node->template is_null<"deleted_at">() || node->template is_null<"node_secret_hash">() ||
        node->template is_null<"agent_id">() || std::string_view(node->template get<"agent_id">()) != value.principal_.agent_id_ ||
        std::string_view(node->template get<"cluster_id">()) != value.principal_.cluster_id_ ||
        std::string_view(node->template get<"node_secret_hash">()) != value.verified_hash_ ||
        (node->template get<"registration_status">() != "registered" &&
            !(value.envelope_.has_authenticate() && node->template get<"registration_status">() == "pending"))) {
        throw invalid_report("report_credentials_expired");
    }
    receipt_entity receipt;
    receipt.template set<"id">(entry.report_id_);
    receipt.template set<"tenant_id">(value.principal_.tenant_id_);
    receipt.template set<"node_id">(value.principal_.node_id_);
    receipt.template set<"agent_id">(value.principal_.agent_id_);
    receipt.template set<"received_at">(received_timestamp(value.received_unix_ms_));
    (void)co_await transaction.template get_repository<receipt_entity>().insert(receipt);
    if (value.envelope_.has_authenticate()) {
        const auto principal = co_await record_authentication(transaction, value.principal_.agent_id_,
            value.verified_hash_, value.received_unix_ms_);
        if (principal.node_id_ != value.principal_.node_id_ || principal.cluster_id_ != value.principal_.cluster_id_ ||
            principal.tenant_id_ != value.principal_.tenant_id_) {
            throw invalid_report("report_authenticated_identity_changed");
        }
    } else if (value.envelope_.has_heartbeat()) {
        co_await record_heartbeat(context, transaction, value.principal_,
            to_heartbeat_report(value.envelope_.heartbeat()), value.received_unix_ms_);
    } else {
        co_await record_apply_result(context, transaction, value.principal_, value.envelope_.apply_result(),
            value.elapsed_milliseconds_, value.trace_);
    }
    co_await transaction.commit();
    service::live_resource::wake_own_outbox(context);
    co_return true;
}

template <typename context_type>
ruvia::task<void> persist_claim(context_type& context, std::string_view consumer, const report_queue::entry& entry) {
    const auto value = report_queue::decode(entry.payload_);
    std::string permanent;
    bool retry{};
    if (!value) {
        permanent = "invalid_report_payload";
    } else {
        try {
            if (!(co_await persist_report(context, consumer, entry, *value))) {
                co_return;
            }
            if (value->envelope_.has_authenticate()) {
                co_await service::node_channel::refresh(context, std::string_view(value->principal_.tenant_id_));
            }
        } catch (const invalid_report& error) {
            permanent = error.what();
        } catch (const ruvia::http_error&) {
            permanent = "report_business_validation_failed";
        } catch (const ruvia::db_error& error) {
            const auto state = error.sql_state();
            if (state && (state->starts_with("22") || state->starts_with("23"))) {
                permanent = "report_data_rejected:" + std::string(*state);
            } else {
                retry = true;
            }
        } catch (const std::exception&) {
            retry = true;
        }
    }
    if (!retry) {
        const auto completed = co_await report_queue::complete(context.redis(), consumer, entry, permanent.empty(),
            permanent, permanent.empty() ? &value->principal_ : nullptr);
        if (completed != 0) {
            co_return;
        }
        service::logging::error("Node report failure archive is full; source retained");
    }
    (void)co_await report_queue::retry(context.redis(), consumer, entry);
}

inline constexpr std::string_view cleanup_finish_script{R"lua(
local source=redis.call('XRANGE',KEYS[1],ARGV[1],ARGV[1],'COUNT',1)
if #source > 0 then
    for i=1,#source[1][2],2 do
        if source[1][2][i]=='report_id' and source[1][2][i+1]==ARGV[3] then return 0 end
    end
end
if redis.call('HGET',KEYS[3],ARGV[1]) ~= ARGV[2] then return 0 end
local remaining = tonumber(redis.call('GET',KEYS[4]) or '0')-string.len(ARGV[1])-string.len(ARGV[2])-128
redis.call('ZREM',KEYS[2],ARGV[1])
redis.call('HDEL',KEYS[3],ARGV[1])
if remaining > 0 then redis.call('SET',KEYS[4],remaining) else redis.call('DEL',KEYS[4]) end
return 1
)lua"};

inline ruvia::task<void> clean_report_consumers(ruvia::redis_handle redis) {
    constexpr std::string_view script{R"lua(
local consumers=redis.call('XINFO','CONSUMERS',KEYS[1],ARGV[1])
for _,fields in ipairs(consumers) do
    local name,pending,idle
    for i=1,#fields,2 do
        if fields[i]=='name' then name=fields[i+1] end
        if fields[i]=='pending' then pending=tonumber(fields[i+1]) end
        if fields[i]=='idle' then idle=tonumber(fields[i+1]) end
    end
    if name and pending==0 and idle and idle>=300000 then redis.call('XGROUP','DELCONSUMER',KEYS[1],ARGV[1],name) end
end
return 1
)lua"};
    const std::array keys{report_queue::stream_key};
    const std::array args{report_queue::group};
    service::utils::require_redis_success(co_await redis.eval(script, keys, args), "could not clean idle node report consumers");
}

// Completion retains a budgeted Redis cleanup record until this transaction
// commits. Process loss before or after either commit is safe to retry.
template <typename context_type>
ruvia::task<std::size_t> clean_report_receipts(context_type& context) {
    const auto candidates = co_await context.redis().command("ZRANGE", report_queue::cleanup_key, "0", "15");
    service::utils::require_redis_success(candidates, "could not inspect node report receipt cleanup");
    std::size_t removed{};
    for (const auto& candidate : candidates.array()) {
        const std::string id(candidate.string());
        const auto identity = co_await context.redis().command("HGET", report_queue::cleanup_identity_key, id);
        service::utils::require_redis_success(identity, "could not read node report cleanup identity");
        if (identity.null()) {
            throw std::runtime_error("node report cleanup identity missing");
        }
        const std::string metadata(identity.string());
        const auto separator = metadata.find('|');
        const auto second_separator = metadata.find('|', separator == std::string::npos ? 0 : separator + 1);
        if (separator == std::string::npos || second_separator == std::string::npos) {
            throw std::runtime_error("invalid node report cleanup identity");
        }
        auto transaction = co_await context.db().begin_transaction();
        (void)co_await lock_report_node(transaction, std::string_view(metadata).substr(0, separator), std::string_view(metadata).substr(separator + 1, second_separator - separator - 1));
        const auto source = co_await context.redis().command("XRANGE", report_queue::stream_key, id, id, "COUNT", "1");
        service::utils::require_redis_success(source, "could not fence node report cleanup");
        bool original_exists{};
        const auto report_id = std::string_view(metadata).substr(second_separator + 1);
        for (const auto& source_entry : source.array()) {
            const auto fields = source_entry.array()[1].array();
            for (std::size_t index = 0; index + 1 < fields.size(); index += 2) {
                if (fields[index].string() == "report_id" && fields[index + 1].string() == report_id) {
                    original_exists = true;
                }
            }
        }
        if (original_exists) {
            continue;
        }
        using entity = report_receipt_entity;
        (void)co_await transaction.template get_repository<entity>().delete_by(entity::column<"id">() == std::string_view(metadata).substr(second_separator + 1));
        co_await transaction.commit();
        const std::array keys{report_queue::stream_key, report_queue::cleanup_key, report_queue::cleanup_identity_key, report_queue::bytes_key};
        const std::array<std::string_view, 3> args{id, metadata, report_id};
        const auto result = co_await context.redis().eval(cleanup_finish_script, keys, args);
        service::utils::require_redis_success(result, "could not complete node report receipt cleanup");
        if (result.integer() == 1) {
            ++removed;
        }
    }
    co_return removed;
}

// A Redis reset may lose cleanup hints after PostgreSQL committed. The bounded
// metadata scan repairs those orphan receipts without assuming an age limit on
// reports that remain queued for an arbitrarily long database outage.
template <typename context_type>
ruvia::task<std::size_t> clean_orphan_receipts(context_type& context, std::string& cursor) {
    using entity = report_receipt_entity;
    const auto candidates = co_await context.db().template get_repository<entity>().find({.where_ = entity::column<"id">() > std::string_view(cursor), .order_ = {{.column_ = "id"}}, .take_ = 16});
    std::size_t removed{};
    if (candidates.empty()) {
        cursor.clear();
    }
    for (const auto& candidate : candidates) {
        const std::string id(candidate.template get<"id">());
        const std::string tenant(candidate.template get<"tenant_id">()), node(candidate.template get<"node_id">());
        auto transaction = co_await context.db().begin_transaction();
        (void)co_await lock_report_node(transaction, tenant, node);
        const auto position = co_await context.redis().command("HGET", report_queue::positions_key, id);
        service::utils::require_redis_success(position, "could not verify orphan report receipt");
        bool live{};
        if (!position.null()) {
            const auto source = co_await context.redis().command("XRANGE", report_queue::stream_key, position.string(), position.string(), "COUNT", "1");
            service::utils::require_redis_success(source, "could not inspect orphan report source");
            for (const auto& entry : source.array()) {
                const auto fields = entry.array()[1].array();
                for (std::size_t index = 0; index + 1 < fields.size(); index += 2) {
                    if (fields[index].string() == "report_id" && fields[index + 1].string() == id) {
                        live = true;
                    }
                }
            }
        }
        if (!live) {
            (void)co_await transaction.template get_repository<entity>().delete_by(entity::column<"id">() == std::string_view(id));
            ++removed;
        }
        co_await transaction.commit();
        cursor = id;
    }
    co_return removed;
}

template <typename context_type>
ruvia::task<void> run_reports(context_type& context) {
    const auto own = service::background::work_available::subscribe(context, report_work_type);
    const auto consumer = service::utils::random_token();
    std::string receipt_cursor;
    auto next_orphan_cleanup = std::chrono::steady_clock::now();
    bool initialized{};
    while (!context.get_stop_token().stop_requested()) {
        bool failed{};
        try {
            if (!initialized) {
                co_await report_queue::initialize(context.redis());
                initialized = true;
            }
            (void)co_await clean_report_receipts(context);
            if (std::chrono::steady_clock::now() >= next_orphan_cleanup) {
                (void)co_await clean_orphan_receipts(context, receipt_cursor);
                next_orphan_cleanup = std::chrono::steady_clock::now() + std::chrono::seconds(15);
            }
            if (auto entry = co_await report_queue::claim(context.redis(), consumer)) {
                co_await persist_claim(context, consumer, *entry);
                continue;
            }
            co_await clean_report_consumers(context.redis());
            const auto duration = co_await report_queue::idle_wait(context.redis());
            (void)co_await own.receive_for(duration, context.get_stop_token());
        } catch (const std::exception&) {
            if (context.get_stop_token().stop_requested()) {
                co_return;
            }
            service::logging::error("Node report persistence interrupted; durable reports retained");
            initialized = false;
            failed = true;
        }
        if (failed) {
            (void)co_await ruvia::sleep_for(context.worker(), std::chrono::seconds(1), context.get_stop_token());
        }
    }
}

}  // namespace service::node_channel
