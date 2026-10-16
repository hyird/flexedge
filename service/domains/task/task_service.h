#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/context.h>
#include <ruvia/web/db/db.h>
#include <ruvia/web/model_json.h>

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "service/domains/task/task_error.h"
#include "service/domains/task/task_mapper.h"
#include "service/domains/task/task_query.h"
#include "service/features/live_resource/fanout.h"

namespace service::task {

class task_service final {
public:
    static constexpr std::int64_t history_limit{50};

    ruvia::task<task_page_dto> list(auto& c, const std::string& tenant, std::int64_t page, std::int64_t page_size,
        std::int64_t skip, const list_filter& filter) {
        // Counts and page belong to the same database snapshot.
        auto transaction = co_await c.db().begin_transaction({.isolation_ = ruvia::db_transaction_isolation::repeatable_read,
            .access_mode_ = ruvia::db_transaction_access_mode::read_only});
        task_query queries(transaction, tenant, c.pool());
        const auto counts = co_await queries.counts(filter).template get_one<task_counts_projection>();
        const auto tasks = co_await queries.list(filter, page_size, skip).template get_many<task_projection>();
        co_await transaction.commit();
        task_page_dto result({.resource_ = c.pool()});
        const auto total = counts->template get<"total">();
        result.template set<"total">(total);
        result.template set<"page">(page);
        result.template set<"page_size">(page_size);
        result.template set<"total_pages">((total + page_size - 1) / page_size);
        result.template set<"active">(counts->template get<"active">());
        result.template set<"failed">(counts->template get<"failed">());
        auto& list = result.template ensure<"list">();
        for (const auto& stored : tasks) {
            list.push_back(to_task(stored, c.pool()));
        }
        co_return result;
    }

    ruvia::task<task_dto> detail(auto& c, const std::string& tenant, const std::string& type, const std::string& id) {
        auto database = c.db();
        task_query queries(database, tenant, c.pool());
        const auto stored = co_await queries.detail(type, id).template get_one<task_projection>();
        if (!stored) {
            service::common::throw_app_error(task_error::not_found);
        }
        co_return to_task(*stored, c.pool());
    }

    ruvia::task<task_history_dto> history(auto& c, const std::string& tenant, const std::string& type,
        const std::string& id) {
        auto transaction = co_await c.db().begin_transaction({.isolation_ = ruvia::db_transaction_isolation::repeatable_read,
            .access_mode_ = ruvia::db_transaction_access_mode::read_only});
        task_query queries(transaction, tenant, c.pool());
        const auto attempts = co_await queries.history(type, id, history_limit).template get_many<attempt_projection>();
        const auto summary = co_await queries.history_summary(type, id).template get_one<history_summary_projection>();
        co_await transaction.commit();
        task_history_dto result({.resource_ = c.pool()});
        auto& list = result.template ensure<"list">();
        for (const auto& stored : attempts) {
            list.push_back(to_attempt(stored, c.pool()));
        }
        result.template set<"summary">(to_history_summary(*summary, c.pool()));
        co_return result;
    }

    // Only the attempts named by durable notices are emitted, in stored order.
    ruvia::task<std::vector<std::string>> recorded_attempts(auto& read, const std::string& tenant,
        const std::string& type, const std::string& id,
        std::span<const service::live_resource::task_attempt_notice> notices) {
        std::vector<std::int64_t> event_ids;
        for (const auto& notice : notices) {
            if (std::find(event_ids.begin(), event_ids.end(), notice.event_id_) == event_ids.end()) {
                event_ids.push_back(notice.event_id_);
            }
        }
        std::vector<std::string> events;
        if (event_ids.empty()) {
            co_return events;
        }
        auto transaction = co_await read.db().begin_transaction({.isolation_ = ruvia::db_transaction_isolation::repeatable_read,
            .access_mode_ = ruvia::db_transaction_access_mode::read_only});
        task_query queries(transaction, tenant, read.pool());
        const auto attempts = co_await queries.recorded_attempts(type, id, event_ids).template get_many<attempt_projection>();
        const auto summary = co_await queries.history_summary(type, id).template get_one<history_summary_projection>();
        co_await transaction.commit();
        for (const auto& stored : attempts) {
            task_attempt_event_dto event({.resource_ = read.pool()});
            event.template set<"item">(to_attempt(stored, read.pool()));
            event.template set<"summary">(to_history_summary(*summary, read.pool()));
            events.emplace_back(ruvia::to_json(event, {.resource_ = read.pool()}));
        }
        co_return events;
    }

    ruvia::task<attempt_trace_dto> attempt_trace(auto& c, const std::string& tenant, const std::string& type,
        const std::string& id, std::int64_t attempt_id) {
        auto database = c.db();
        task_query queries(database, tenant, c.pool());
        const auto stored = co_await queries.attempt_trace(type, id, attempt_id).template get_one<attempt_trace_projection>();
        if (!stored) {
            service::common::throw_app_error(task_error::attempt_not_found);
        }
        co_return to_attempt_trace(*stored, c.pool());
    }
};

inline task_service& get_task_service() {
    static task_service service;
    return service;
}

}  // namespace service::task
