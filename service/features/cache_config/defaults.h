#pragma once

#include <string_view>

#include "common/cache_defaults.h"

namespace service::cache_config {

inline constexpr std::string_view default_policy_name{"默认缓存策略"};
inline constexpr std::string_view default_policy_description{
    "安全静态预设：仅缓存静态文件白名单，未匹配请求绕过缓存，遵循源站缓存限制，启用缓存欺骗防护。"};

// Initial values only: persisted policies own their configuration after creation.
inline flexedge::cache::policy default_policy() {
    flexedge::cache::policy result;
    result.memory_percent_ = 50;
    result.disk_percent_ = 50;
    result.memory_reserved_bytes_ = 512 * 1024 * 1024;
    result.disk_reserved_bytes_ = std::int64_t{5} * 1024 * 1024 * 1024;
    result.rules_ = flexedge::cache::safe_static_rules();
    return result;
}

}  // namespace service::cache_config
