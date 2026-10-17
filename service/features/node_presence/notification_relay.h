#pragma once

#include "service/features/live_resource/notification_outbox.h"
#include "service/features/node_presence/node_presence.h"

namespace service::node_presence {

// Resource events carry node heartbeat/runtime patches. Their owner publishes
// the patch, cache invalidations and online deadline together, including replay
// fencing. The generic outbox only owns durable claiming and retry.
struct resource_relay final {
    static constexpr std::string_view kind = service::live_resource::outbox::resource_kind;

    static ruvia::task<void> publish(ruvia::redis_handle redis, std::string_view id, std::string_view payload) {
        namespace outbox = service::live_resource::outbox;
        auto fields = outbox::unpack(payload);
        auto notification = notifications::decode(fields);
        if (!notification) {
            throw std::runtime_error("invalid resource outbox notification");
        }
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
                             .count();
        if (!notification->node_id_.empty() && notification->node_seen_unix_ms_ > 0 &&
            now - notification->node_seen_unix_ms_ >
                std::chrono::duration_cast<std::chrono::milliseconds>(service::node::online_window).count()) {
            // A delayed patch cannot replace newer runtime. Keep its original
            // heartbeat timestamp and request a recovery snapshot instead.
            notification->node_runtime_.clear();
            notification->origin_runtime_.clear();
            notification->missed_until_ = now;
        }
        if (notification->missed_until_) {
            notification->missed_until_ = now;
            fields = notifications::encode(*notification);
        }
        std::vector<std::string> invalidations;
        for (const auto& change : notification->changes_) {
            auto scopes = redis_snapshot_cache::invalidation_scopes(notification->tenant_id_, change.resource_, change.id_);
            invalidations.insert(invalidations.end(), scopes.begin(), scopes.end());
        }
        if (notification->missed_until_) {
            invalidations.push_back(notification->tenant_id_ == notifications::all_tenants
                                        ? "*"
                                        : redis_snapshot_cache::tenant_scope(notification->tenant_id_));
        }
        co_await outbox::publish(redis, id,
            {.stream_ = notifications::stream_key,
                .max_entries_ = notifications::max_entries,
                .fields_ = std::move(fields),
                .invalidations_ = std::move(invalidations),
                .extra_keys_ = {std::string(deadlines_key)},
                .extra_arguments_ = {notification->tenant_id_, notification->node_id_,
                    std::to_string(notification->node_seen_unix_ms_)},
                .helper_lua_ = record_lua,
                .pre_publish_lua_ = validate_presence_script,
                .post_publish_lua_ = record_presence_script});
    }

private:
    static constexpr std::string_view validate_presence_script{R"lua(
local has_presence = ARGV[#ARGV-2] ~= ''
if has_presence then
    presence_validate(KEYS[6])
    local tenant, node, seen = ARGV[#ARGV-3], ARGV[#ARGV-2], tonumber(ARGV[#ARGV-1])
    if tenant == '' or string.find(tenant, '|', 1, true) or string.find(node, '|', 1, true) or not seen or seen <= 0 then
        return redis.error_reply('invalid presence metadata')
    end
end
)lua"};

    static constexpr std::string_view record_presence_script{R"lua(
if has_presence then presence_record(KEYS[6], ARGV[#ARGV-3], ARGV[#ARGV-2], tonumber(ARGV[#ARGV-1])) end
)lua"};
};

}  // namespace service::node_presence
