#pragma once

#include <string_view>

#include "common/cache_policy.h"

namespace service::cache_config {

inline constexpr std::string_view default_policy_name{"默认缓存策略"};
inline constexpr std::string_view default_policy_description{
    "系统预置策略，可直接编辑。静态资源缓存一天。"};

// Initial values only: persisted policies own their configuration after creation.
inline flexedge::cache::policy default_policy() {
    flexedge::cache::policy result;
    result.memory_percent_ = 50;
    result.disk_percent_ = 50;
    result.memory_reserved_bytes_ = 512 * 1024 * 1024;
    result.disk_reserved_bytes_ = std::int64_t{5} * 1024 * 1024 * 1024;
    result.rules_ = {
        {.id_ = "cache-static",
            .name_ = "静态资源缓存一天",
            .patterns_ = {".html", ".css", ".js", ".mjs", ".jpg", ".jpeg", ".png", ".gif", ".webp", ".avif",
                ".svg", ".ico", ".woff", ".woff2", ".ttf", ".otf"},
            .ttl_mode_ = "fixed",
            .ttl_seconds_ = 86400},
    };
    return result;
}

}  // namespace service::cache_config
