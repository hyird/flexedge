#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/redis/redis.h>

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "service/features/live_resource/notification_outbox.h"
#include "service/features/live_resource/redis_snapshot_cache.h"
#include "service/features/log_ingest/notifications.h"

namespace service::log_ingest {

// Rows of outbox kind "log" carry log notifications of committed log rows.
struct log_relay final {
    static constexpr std::string_view kind{"log"};

    static ruvia::task<void> publish(ruvia::redis_handle redis, std::string_view id, std::string_view payload) {
        namespace live = service::live_resource;
        live::outbox::publication output{.stream_ = notifications::stream_key, .max_entries_ = notifications::max_entries};
        output.fields_ = live::outbox::unpack(payload);
        std::string_view tenant, type, resource_id;
        for (const auto& [name, value] : output.fields_) {
            if (name == "tenant_id") {
                tenant = value;
            }
            if (name == "resource_type") {
                type = value;
            }
            if (name == "resource_id") {
                resource_id = value;
            }
        }
        if (tenant.empty() || resource_id.empty() || (type != "access" && type != "node")) {
            throw std::runtime_error("invalid log outbox notification");
        }
        if (type == "access") {
            output.invalidations_ = live::redis_snapshot_cache::invalidation_scopes(tenant, live::resource::access_history, resource_id);
            output.invalidations_.push_back(live::redis_snapshot_cache::resource_scope(tenant, live::resource::overview_traffic));
        }
        co_await live::outbox::publish(redis, id, std::move(output));
    }
};

template <typename transaction_type>
ruvia::task<void> enqueue_log(transaction_type& transaction, std::string_view type,
    std::string_view tenant_id, std::string_view resource_id) {
    if (type != "access" && type != "node") {
        throw std::invalid_argument("unsupported outbox log resource");
    }
    const std::vector<std::pair<std::string, std::string>> fields{
        {"tenant_id", std::string(tenant_id)}, {"resource_type", std::string(type)}, {"resource_id", std::string(resource_id)}};
    (void)co_await service::live_resource::outbox::insert(transaction, log_relay::kind,
        service::live_resource::outbox::pack(fields));
}

}  // namespace service::log_ingest
