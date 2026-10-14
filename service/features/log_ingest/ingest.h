#pragma once

#include <ruvia/core/task.h>
#include <ruvia/core/timer.h>
#include <ruvia/web/context.h>
#include <ruvia/web/db/db.h>
#include <ruvia/web/redis/redis.h>
#include <ruvia/web/web_worker.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "service/features/log_ingest/envelope.h"
#include "service/features/log_ingest/notifications.h"
#include "service/features/log_ingest/queue.h"
#include "service/features/logging/logger.h"

namespace service::log_ingest {

inline std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch())
        .count();
}

inline std::int64_t bounded_occurred_ms(std::uint64_t value) {
    const auto now = now_ms();
    const auto maximum = static_cast<std::uint64_t>(now + 5 * 60 * 1000);
    if (value == 0 || value > maximum) {
        return now;
    }
    return static_cast<std::int64_t>(value);
}

inline ruvia::task<bool> enqueue(ruvia::context& c, std::string_view tenant_id,
    std::string_view node_id, std::string_view agent_id,
    const flexedge::node::v2::LogDelivery& delivery) {
    try {
        if (!valid_delivery(delivery, node_id)) {
            co_return false;
        }
        const auto payload = serialize_envelope(tenant_id, agent_id, delivery);
        co_return co_await queue::push(c.redis(), payload);
    } catch (const std::exception& error) {
        service::logging::error("Log enqueue failed: " + std::string(error.what()));
        co_return false;
    } catch (...) {
        service::logging::error("Log enqueue failed with unknown exception");
        co_return false;
    }
}

template <typename context_t_type>
inline ruvia::task<void> insert_envelope(context_t_type& context,
    const v2::QueuedLogDelivery& envelope) {
    if (!valid_envelope(envelope)) {
        throw std::runtime_error("invalid queued log delivery");
    }
    const auto& delivery = envelope.delivery();
    const std::string_view tenant_id{envelope.tenant_id()};
    const std::string_view node_id{delivery.node_id()};
    const std::string_view agent_id{envelope.agent_id()};
    auto db = context.db();
    auto transaction = co_await db.begin_transaction();
    const auto node_rows = co_await transaction.query(
        "SELECT cluster_id FROM sys_node WHERE tenant_id = $1 AND id = $2 AND agent_id = $3 AND "
        "registration_status = 'registered' AND deleted_at IS NULL LIMIT 1 FOR SHARE",
        tenant_id, node_id, agent_id);
    if (node_rows.empty()) {
        co_await transaction.commit();
        co_return;
    }
    const auto cluster_id = std::string(node_rows.front()[0].value().value_or(""));
    std::vector<ruvia::db_value> access_params;
    std::vector<ruvia::db_value> node_params;
    access_params.reserve(static_cast<std::size_t>(delivery.events_size()) * 22);
    node_params.reserve(static_cast<std::size_t>(delivery.events_size()) * 8);
    std::vector<std::string> access_website_ids;
    access_website_ids.reserve(static_cast<std::size_t>(delivery.events_size()));
    std::string access_sql =
        "INSERT INTO sys_website_access_log (tenant_id, occurred_at, id, node_id, website_id, "
        "client_ip, protocol, method, host, target, status_code, request_bytes, response_bytes, "
        "duration_ms, "
        "user_agent, referer, request_headers, request_body, request_body_truncated, "
        "tls_fingerprint, response_headers, query_string, cookies, created_at) VALUES ";
    std::string node_sql =
        "INSERT INTO sys_node_log (tenant_id, occurred_at, id, node_id, level, category, message, "
        "created_at) VALUES ";
    const auto append_param = [](std::string& sql, std::vector<ruvia::db_value>& params,
                                  ruvia::db_value value) {
        sql += "$" + std::to_string(params.size() + 1);
        params.push_back(std::move(value));
    };
    const auto append_text = [&](std::string& sql, std::vector<ruvia::db_value>& params,
                                 std::string_view value) {
        append_param(sql, params, ruvia::db_value{value});
    };
    std::size_t access_count{};
    std::size_t node_count{};
    for (const auto& event : delivery.events()) {
        if (event.has_access_log()) {
            const auto& access = event.access_log();
            if (access_count++ > 0) {
                access_sql += ", ";
            }
            access_sql += "(";
            append_text(access_sql, access_params, tenant_id);
            access_sql += ", TO_TIMESTAMP(";
            append_param(access_sql, access_params,
                ruvia::db_value{bounded_occurred_ms(access.occurred_unix_ms())});
            access_sql += "::double precision / 1000.0), ";
            append_text(access_sql, access_params, access.id());
            access_sql += ", ";
            append_text(access_sql, access_params, node_id);
            access_sql += ", ";
            append_text(access_sql, access_params, access.website_id());
            access_website_ids.emplace_back(access.website_id());
            access_sql += ", ";
            append_text(access_sql, access_params, access.client_ip());
            access_sql += ", ";
            append_text(access_sql, access_params, access.protocol());
            access_sql += ", ";
            append_text(access_sql, access_params, access.method());
            access_sql += ", ";
            append_text(access_sql, access_params, access.host());
            access_sql += ", ";
            append_text(access_sql, access_params, access.target());
            access_sql += ", ";
            append_param(access_sql, access_params,
                ruvia::db_value{static_cast<std::int64_t>(access.status_code())});
            access_sql += ", ";
            append_param(access_sql, access_params,
                ruvia::db_value{static_cast<std::int64_t>(access.request_bytes())});
            access_sql += ", ";
            append_param(access_sql, access_params,
                ruvia::db_value{static_cast<std::int64_t>(access.response_bytes())});
            access_sql += ", ";
            append_param(access_sql, access_params,
                ruvia::db_value{static_cast<std::int64_t>(access.duration_ms())});
            access_sql += ", ";
            append_text(access_sql, access_params, access.user_agent());
            access_sql += ", ";
            append_text(access_sql, access_params, access.referer());
            access_sql += ", ";
            append_text(access_sql, access_params, access.request_headers());
            access_sql += ", ";
            append_text(access_sql, access_params, access.request_body());
            access_sql += ", ";
            append_param(access_sql, access_params, ruvia::db_value{access.request_body_truncated()});
            access_sql += ", ";
            append_text(access_sql, access_params, access.tls_fingerprint());
            access_sql += ", ";
            append_text(access_sql, access_params, access.response_headers());
            access_sql += ", ";
            append_text(access_sql, access_params, access.query_string());
            access_sql += ", ";
            append_text(access_sql, access_params, access.cookies());
            access_sql += ", NOW())";
            continue;
        }

        const auto& item = event.node_log();
        if (node_count++ > 0) {
            node_sql += ", ";
        }
        node_sql += "(";
        append_text(node_sql, node_params, tenant_id);
        node_sql += ", TO_TIMESTAMP(";
        append_param(node_sql, node_params,
            ruvia::db_value{bounded_occurred_ms(item.occurred_unix_ms())});
        node_sql += "::double precision / 1000.0), ";
        append_text(node_sql, node_params, item.id());
        node_sql += ", ";
        append_text(node_sql, node_params, node_id);
        node_sql += ", ";
        append_text(node_sql, node_params, item.level());
        node_sql += ", ";
        append_text(node_sql, node_params, item.category());
        node_sql += ", ";
        append_text(node_sql, node_params, item.message());
        node_sql += ", NOW())";
    }
    std::sort(access_website_ids.begin(), access_website_ids.end());
    access_website_ids.erase(std::unique(access_website_ids.begin(), access_website_ids.end()),
        access_website_ids.end());
    if (!access_website_ids.empty()) {
        std::vector<ruvia::db_value> website_params;
        website_params.reserve(access_website_ids.size() + 2);
        website_params.emplace_back(tenant_id);
        website_params.emplace_back(cluster_id);
        std::string placeholders;
        for (const auto& website_id : access_website_ids) {
            if (!placeholders.empty()) {
                placeholders += ", ";
            }
            placeholders += "$" + std::to_string(website_params.size() + 1);
            website_params.emplace_back(website_id);
        }
        const auto website_rows = co_await transaction.query(
            "SELECT id FROM sys_website WHERE tenant_id = $1 AND cluster_id = $2 AND "
            "deleted_at IS NULL AND id IN (" +
                placeholders + ") FOR SHARE",
            website_params);
        if (website_rows.size() != access_website_ids.size()) {
            service::logging::error("Dropped log delivery with invalid website scope");
            co_await transaction.commit();
            co_return;
        }
    }
    if (access_count > 0) {
        access_sql += " ON CONFLICT DO NOTHING";
        (void)co_await transaction.execute(access_sql, access_params);
    }
    if (node_count > 0) {
        node_sql += " ON CONFLICT DO NOTHING";
        (void)co_await transaction.execute(node_sql, node_params);
    }
    co_await transaction.commit();
    try {
        for (const auto& website_id : access_website_ids) {
            co_await notifications::publish(context.redis(), notifications::log_resource_type::access,
                tenant_id, website_id);
        }
        if (node_count > 0) {
            co_await notifications::publish(context.redis(), notifications::log_resource_type::node,
                tenant_id, node_id);
        }
    } catch (const std::exception& error) {
        service::logging::error("Log notification failed: " + std::string(error.what()));
    } catch (...) {
        service::logging::error("Log notification failed with unknown exception");
    }
}

template <typename context_t_type>
inline ruvia::task<std::size_t> process(context_t_type& context, std::string_view consumer,
    std::vector<queue::entry> entries) {
    std::size_t processed{};
    for (const auto& entry : entries) {
        const auto envelope = entry.valid_ ? parse_envelope(entry.payload_) : std::nullopt;
        if (!envelope) {
            service::logging::error("Dropped malformed queued log delivery");
            co_await queue::finish(context.redis(), consumer, entry);
            continue;
        }
        bool insert_failed{};
        try {
            co_await insert_envelope(context, *envelope);
        } catch (...) {
            insert_failed = true;
        }
        if (insert_failed) {
            const auto delivery_count =
                co_await queue::delivery_count(context.redis(), consumer, entry.id_);
            if (delivery_count) {
                if (*delivery_count >= queue::max_delivery_attempts) {
                    service::logging::error("Dropped log delivery after maximum delivery attempts");
                    co_await queue::finish(context.redis(), consumer, entry);
                } else {
                    service::logging::error("Log delivery insert failed; delivery will be retried");
                }
            }
            continue;
        }
        co_await queue::finish(context.redis(), consumer, entry);
        ++processed;
    }
    co_return processed;
}

template <typename context_t_type>
inline ruvia::task<void> recover_stale(context_t_type& context, std::string_view consumer) {
    std::string cursor{"0-0"};
    do {
        auto claimed = co_await queue::auto_claim(context.redis(), consumer, cursor);
        cursor = std::move(claimed.next_cursor_);
        if (!claimed.entries_.empty()) {
            (void)co_await process(context, consumer, std::move(claimed.entries_));
        }
    } while (cursor != "0-0");
}

template <typename context_t_type>
inline ruvia::task<void> run_worker(context_t_type& context, std::string consumer) {
    if (consumer.empty()) {
        throw std::invalid_argument("log ingest consumer name cannot be empty");
    }
    bool initialized{};
    auto next_recovery = std::chrono::steady_clock::now();
    while (!context.get_stop_token().stop_requested()) {
        bool failed{};
        try {
            if (!initialized) {
                co_await queue::initialize(context.redis());
                initialized = true;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= next_recovery) {
                co_await recover_stale(context, consumer);
                next_recovery = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            }
            auto entries = co_await queue::read(context.redis(), consumer);
            if (!entries.empty()) {
                (void)co_await process(context, consumer, std::move(entries));
            }
        } catch (const std::exception& error) {
            if (context.get_stop_token().stop_requested()) {
                co_return;
            }
            initialized = false;
            failed = true;
            service::logging::error("Log ingest worker failure: " + std::string(error.what()));
        } catch (...) {
            if (context.get_stop_token().stop_requested()) {
                co_return;
            }
            initialized = false;
            failed = true;
            service::logging::error("Log ingest worker failure with unknown exception");
        }
        if (failed && co_await ruvia::sleep_for(context.worker(), std::chrono::seconds(1),
                          context.get_stop_token()) ==
                          ruvia::timer_sleep_result::stop_requested) {
            co_return;
        }
    }
}

}  // namespace service::log_ingest
