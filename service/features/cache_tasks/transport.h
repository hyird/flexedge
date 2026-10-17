#pragma once
#include <ruvia/core/task.h>
#include <ruvia/web/redis/redis.h>

#include <chrono>

#include "common/hex.h"
#include "node/proto/edge_control.pb.h"
#include "service/utils/redis_reply.h"
namespace service::cache_tasks {
inline std::string command_key(std::string_view node) {
    return "flexedge:cache-commands:v1:" + std::string(node);
}
inline std::string encode(const flexedge::node::v2::CacheCommand& command) {
    const auto bytes = command.SerializeAsString();
    return flexedge::crypto::hex_encode({reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size()});
}
inline std::optional<flexedge::node::v2::CacheCommand> decode(std::string_view text) {
    if (text.size() > 1024 * 1024) {
        return {};
    }
    const auto bytes = flexedge::crypto::hex_decode(text);
    flexedge::node::v2::CacheCommand command;
    if (!bytes || !command.ParseFromArray(bytes->data(), static_cast<int>(bytes->size()))) {
        return {};
    }
    return command;
}
inline ruvia::task<void> enqueue(ruvia::redis_handle redis, std::string_view node,
    const flexedge::node::v2::CacheCommand& command) {
    const auto key = command_key(node);
    const auto result = co_await redis.command("HSET", key, command.task_id(), encode(command));
    service::utils::require_redis_success(result, "could not publish cache command");
    service::utils::require_redis_success(co_await redis.command("EXPIRE", key, "600"), "could not expire cache commands");
}

inline ruvia::task<void> acknowledge(ruvia::redis_handle redis, std::string_view node,
    const flexedge::node::v2::CacheResult& result) {
    const auto key = command_key(node);
    const auto current = co_await redis.command("HGET", key, result.task_id());
    service::utils::require_redis_success(current, "could not read completed cache command");
    if (current.null()) {
        co_return;
    }
    const auto command = decode(current.string());
    if (!command || command->attempt_id() != result.attempt_id() || command->generation() != result.generation()) {
        co_return;
    }
    constexpr std::string_view script = R"lua(
if redis.call('HGET',KEYS[1],ARGV[1]) == ARGV[2] then return redis.call('HDEL',KEYS[1],ARGV[1]) end
return 0
)lua";
    const std::array<std::string_view, 1> keys{key};
    const std::array<std::string_view, 2> args{result.task_id(), current.string()};
    service::utils::require_redis_success(co_await redis.eval(script, keys, args), "could not acknowledge cache command");
}
inline ruvia::task<std::vector<flexedge::node::v2::CacheCommand>> pending(ruvia::redis_handle redis, std::string_view node) {
    std::vector<flexedge::node::v2::CacheCommand> commands;
    const auto values = co_await redis.command("HVALS", command_key(node));
    service::utils::require_redis_success(values, "could not read cache commands");
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    for (const auto& value : values.array()) {
        auto command = decode(value.string());
        if (command && command->expires_unix_ms() > now && commands.size() < 8) {
            commands.push_back(std::move(*command));
        }
    }
    co_return commands;
}
}  // namespace service::cache_tasks
