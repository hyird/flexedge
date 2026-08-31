#pragma once

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

#include <ruvia/core/Task.h>
#include <ruvia/core/Timer.h>
#include <ruvia/web/Context.h>
#include <ruvia/web/WebWorker.h>
#include <ruvia/web/db/Db.h>
#include <ruvia/web/redis/Redis.h>

#include "service/features/log_ingest/envelope.h"
#include "service/features/log_ingest/notifications.h"
#include "service/features/log_ingest/queue.h"
#include "service/features/logging/logger.h"

namespace service::log_ingest {

inline std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

inline std::int64_t boundedOccurredMs(std::uint64_t value) {
    const auto now = nowMs();
    const auto maximum = static_cast<std::uint64_t>(now + 5 * 60 * 1000);
    if (value == 0 || value > maximum) {
        return now;
    }
    return static_cast<std::int64_t>(value);
}

inline ruvia::Task<bool> enqueue(ruvia::Context& c, std::string_view tenantId,
                                 std::string_view nodeId, std::string_view agentId,
                                 const flexedge::node::v2::LogDelivery& delivery) {
    try {
        if (!validDelivery(delivery, nodeId)) {
            co_return false;
        }
        const auto payload = serializeEnvelope(tenantId, agentId, delivery);
        co_return co_await queue::push(c.redis(), payload);
    } catch (const std::exception& error) {
        service::logging::error("Log enqueue failed: " + std::string(error.what()));
        co_return false;
    } catch (...) {
        service::logging::error("Log enqueue failed with unknown exception");
        co_return false;
    }
}

inline ruvia::Task<void> insertEnvelope(ruvia::WebWorkerContext& context,
                                        const v2::QueuedLogDelivery& envelope) {
    if (!validEnvelope(envelope)) {
        throw std::runtime_error("invalid queued log delivery");
    }
    const auto& delivery = envelope.delivery();
    const std::string_view tenantId{envelope.tenant_id()};
    const std::string_view nodeId{delivery.node_id()};
    const std::string_view agentId{envelope.agent_id()};
    auto transaction = co_await context.db().beginTransaction();
    const auto nodeRows = co_await transaction.query(
        "SELECT cluster_id FROM sys_node WHERE tenant_id = $1 AND id = $2 AND agent_id = $3 AND "
        "registration_status = 'registered' AND deleted_at IS NULL LIMIT 1 FOR SHARE",
        tenantId, nodeId, agentId);
    if (nodeRows.empty()) {
        co_await transaction.commit();
        co_return;
    }
    const auto clusterId = std::string(nodeRows.front()[0].value().value_or(""));
    std::vector<ruvia::DbValue> accessParams;
    std::vector<ruvia::DbValue> nodeParams;
    accessParams.reserve(static_cast<std::size_t>(delivery.events_size()) * 22);
    nodeParams.reserve(static_cast<std::size_t>(delivery.events_size()) * 8);
    std::vector<std::string> accessWebsiteIds;
    accessWebsiteIds.reserve(static_cast<std::size_t>(delivery.events_size()));
    std::string accessSql =
        "INSERT INTO sys_website_access_log (tenant_id, occurred_at, id, node_id, website_id, "
        "client_ip, protocol, method, host, target, status_code, response_bytes, duration_ms, "
        "user_agent, referer, request_headers, request_body, request_body_truncated, "
        "tls_fingerprint, response_headers, query_string, cookies, created_at) VALUES ";
    std::string nodeSql =
        "INSERT INTO sys_node_log (tenant_id, occurred_at, id, node_id, level, category, message, "
        "created_at) VALUES ";
    const auto appendParam = [](std::string& sql, std::vector<ruvia::DbValue>& params,
                                ruvia::DbValue value) {
        sql += "$" + std::to_string(params.size() + 1);
        params.push_back(std::move(value));
    };
    const auto appendText = [&](std::string& sql, std::vector<ruvia::DbValue>& params,
                                std::string_view value) {
        appendParam(sql, params, ruvia::DbValue{value});
    };
    std::size_t accessCount{};
    std::size_t nodeCount{};
    for (const auto& event : delivery.events()) {
        if (event.has_access_log()) {
            const auto& access = event.access_log();
            if (accessCount++ > 0) {
                accessSql += ", ";
            }
            accessSql += "(";
            appendText(accessSql, accessParams, tenantId);
            accessSql += ", TO_TIMESTAMP(";
            appendParam(accessSql, accessParams,
                        ruvia::DbValue{boundedOccurredMs(access.occurred_unix_ms())});
            accessSql += "::double precision / 1000.0), ";
            appendText(accessSql, accessParams, access.id());
            accessSql += ", ";
            appendText(accessSql, accessParams, nodeId);
            accessSql += ", ";
            appendText(accessSql, accessParams, access.website_id());
            accessWebsiteIds.emplace_back(access.website_id());
            accessSql += ", ";
            appendText(accessSql, accessParams, access.client_ip());
            accessSql += ", ";
            appendText(accessSql, accessParams, access.protocol());
            accessSql += ", ";
            appendText(accessSql, accessParams, access.method());
            accessSql += ", ";
            appendText(accessSql, accessParams, access.host());
            accessSql += ", ";
            appendText(accessSql, accessParams, access.target());
            accessSql += ", ";
            appendParam(accessSql, accessParams,
                        ruvia::DbValue{static_cast<std::int64_t>(access.status_code())});
            accessSql += ", ";
            appendParam(accessSql, accessParams,
                        ruvia::DbValue{static_cast<std::int64_t>(access.response_bytes())});
            accessSql += ", ";
            appendParam(accessSql, accessParams,
                        ruvia::DbValue{static_cast<std::int64_t>(access.duration_ms())});
            accessSql += ", ";
            appendText(accessSql, accessParams, access.user_agent());
            accessSql += ", ";
            appendText(accessSql, accessParams, access.referer());
            accessSql += ", ";
            appendText(accessSql, accessParams, access.request_headers());
            accessSql += ", ";
            appendText(accessSql, accessParams, access.request_body());
            accessSql += ", ";
            appendParam(accessSql, accessParams, ruvia::DbValue{access.request_body_truncated()});
            accessSql += ", ";
            appendText(accessSql, accessParams, access.tls_fingerprint());
            accessSql += ", ";
            appendText(accessSql, accessParams, access.response_headers());
            accessSql += ", ";
            appendText(accessSql, accessParams, access.query_string());
            accessSql += ", ";
            appendText(accessSql, accessParams, access.cookies());
            accessSql += ", NOW())";
            continue;
        }

        const auto& item = event.node_log();
        if (nodeCount++ > 0) {
            nodeSql += ", ";
        }
        nodeSql += "(";
        appendText(nodeSql, nodeParams, tenantId);
        nodeSql += ", TO_TIMESTAMP(";
        appendParam(nodeSql, nodeParams,
                    ruvia::DbValue{boundedOccurredMs(item.occurred_unix_ms())});
        nodeSql += "::double precision / 1000.0), ";
        appendText(nodeSql, nodeParams, item.id());
        nodeSql += ", ";
        appendText(nodeSql, nodeParams, nodeId);
        nodeSql += ", ";
        appendText(nodeSql, nodeParams, item.level());
        nodeSql += ", ";
        appendText(nodeSql, nodeParams, item.category());
        nodeSql += ", ";
        appendText(nodeSql, nodeParams, item.message());
        nodeSql += ", NOW())";
    }
    std::sort(accessWebsiteIds.begin(), accessWebsiteIds.end());
    accessWebsiteIds.erase(std::unique(accessWebsiteIds.begin(), accessWebsiteIds.end()),
                           accessWebsiteIds.end());
    if (!accessWebsiteIds.empty()) {
        std::vector<ruvia::DbValue> websiteParams;
        websiteParams.reserve(accessWebsiteIds.size() + 2);
        websiteParams.emplace_back(tenantId);
        websiteParams.emplace_back(clusterId);
        std::string placeholders;
        for (const auto& websiteId : accessWebsiteIds) {
            if (!placeholders.empty()) {
                placeholders += ", ";
            }
            placeholders += "$" + std::to_string(websiteParams.size() + 1);
            websiteParams.emplace_back(websiteId);
        }
        const auto websiteRows = co_await transaction.query(
            "SELECT id FROM sys_website WHERE tenant_id = $1 AND cluster_id = $2 AND "
            "deleted_at IS NULL AND id IN (" +
                placeholders + ") FOR SHARE",
            websiteParams);
        if (websiteRows.size() != accessWebsiteIds.size()) {
            service::logging::error("Dropped log delivery with invalid website scope");
            co_await transaction.commit();
            co_return;
        }
    }
    if (accessCount > 0) {
        accessSql += " ON CONFLICT DO NOTHING";
        (void)co_await transaction.execute(accessSql, accessParams);
    }
    if (nodeCount > 0) {
        nodeSql += " ON CONFLICT DO NOTHING";
        (void)co_await transaction.execute(nodeSql, nodeParams);
    }
    co_await transaction.commit();
    try {
        for (const auto& websiteId : accessWebsiteIds) {
            co_await notifications::publishAccess(context.redis(), tenantId, websiteId);
        }
        if (nodeCount > 0) {
            co_await notifications::publishNode(context.redis(), tenantId, nodeId);
        }
    } catch (const std::exception& error) {
        service::logging::error("Log notification failed: " + std::string(error.what()));
    } catch (...) {
        service::logging::error("Log notification failed with unknown exception");
    }
}

inline ruvia::Task<std::size_t> process(ruvia::WebWorkerContext& context, std::string_view consumer,
                                        std::vector<queue::Entry> entries) {
    std::size_t processed{};
    for (const auto& entry : entries) {
        const auto envelope = entry.valid ? parseEnvelope(entry.payload) : std::nullopt;
        if (!envelope) {
            service::logging::error("Dropped malformed queued log delivery");
            co_await queue::finish(context.redis(), consumer, entry);
            continue;
        }
        bool insertFailed{};
        try {
            co_await insertEnvelope(context, *envelope);
        } catch (...) {
            insertFailed = true;
        }
        if (insertFailed) {
            const auto deliveryCount =
                co_await queue::deliveryCount(context.redis(), consumer, entry.id);
            if (deliveryCount) {
                if (*deliveryCount >= queue::kMaxDeliveryAttempts) {
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

inline ruvia::Task<void> recoverStale(ruvia::WebWorkerContext& context, std::string_view consumer) {
    std::string cursor{"0-0"};
    do {
        auto claimed = co_await queue::autoClaim(context.redis(), consumer, cursor);
        cursor = std::move(claimed.nextCursor);
        if (!claimed.entries.empty()) {
            (void)co_await process(context, consumer, std::move(claimed.entries));
        }
    } while (cursor != "0-0");
}

inline ruvia::Task<void> runWorker(ruvia::WebWorkerContext& context, std::string consumer) {
    if (consumer.empty()) {
        throw std::invalid_argument("log ingest consumer name cannot be empty");
    }
    bool initialized{};
    auto nextRecovery = std::chrono::steady_clock::now();
    while (!context.stopToken().stopRequested()) {
        bool failed{};
        try {
            if (!initialized) {
                co_await queue::initialize(context.redis());
                initialized = true;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= nextRecovery) {
                co_await recoverStale(context, consumer);
                nextRecovery = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            }
            auto entries = co_await queue::read(context.redis(), consumer);
            if (!entries.empty()) {
                (void)co_await process(context, consumer, std::move(entries));
            }
        } catch (const std::exception& error) {
            if (context.stopToken().stopRequested()) {
                co_return;
            }
            initialized = false;
            failed = true;
            service::logging::error("Log ingest worker failure: " + std::string(error.what()));
        } catch (...) {
            if (context.stopToken().stopRequested()) {
                co_return;
            }
            initialized = false;
            failed = true;
            service::logging::error("Log ingest worker failure with unknown exception");
        }
        if (failed && co_await ruvia::sleepFor(context.worker(), std::chrono::seconds(1),
                                               context.stopToken()) ==
                          ruvia::TimerSleepResult::kStopRequested) {
            co_return;
        }
    }
}

} // namespace service::log_ingest
