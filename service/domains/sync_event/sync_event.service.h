#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include <ruvia/core/Task.h>
#include <ruvia/web/Context.h>

#include "service/domains/sync_event/sync_event.types.h"

namespace service::sync_event {

class SyncEventService final {
  public:
    static constexpr std::int64_t kPageSize{100};

    ruvia::Task<SyncEventPageDataDto> list(ruvia::Context& c, const std::string& tenantId,
                                           const std::optional<std::int64_t>& after) {
        SyncEventPageDataDto result(c);
        auto& items = result.ensure<"list">();
        if (!after) {
            const auto cursorRows = co_await c.db().query(
                "SELECT COALESCE(MAX(id), 0) FROM sys_sync_event WHERE tenant_id = $1", tenantId);
            result.set<"cursor">(cursorRows.empty()
                                     ? std::int64_t{0}
                                     : cursorRows.front()[0].as<std::int64_t>().value_or(0));
            result.set<"hasMore">(false);
            co_return result;
        }

        const auto rows = co_await c.db().query(
            "SELECT id, resource_type, resource_id, operation, version, outcome, TO_CHAR("
            "emitted_at, 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF') FROM sys_sync_event WHERE "
            "tenant_id = $1 AND id > $2 ORDER BY id ASC LIMIT " +
                std::to_string(kPageSize + 1),
            tenantId, *after);
        std::int64_t cursor = *after;
        std::size_t count = 0;
        for (const auto& row : rows) {
            if (count++ == static_cast<std::size_t>(kPageSize)) {
                break;
            }
            cursor = row[0].as<std::int64_t>().value_or(cursor);
            auto& item = items.emplace_back(c);
            item.set<"sequence">(cursor);
            item.set<"resourceType">(row[1].value().value_or(""));
            item.set<"resourceId">(row[2].value().value_or(""));
            item.set<"operation">(row[3].value().value_or(""));
            item.set<"version">(row[4].as<std::int64_t>().value_or(0));
            item.set<"outcome">(row[5].value().value_or(""));
            item.set<"emittedAt">(row[6].value().value_or(""));
        }
        result.set<"cursor">(cursor);
        result.set<"hasMore">(rows.size() > static_cast<std::size_t>(kPageSize));
        co_return result;
    }
};

inline SyncEventService& syncEventService() {
    static SyncEventService instance;
    return instance;
}

} // namespace service::sync_event
