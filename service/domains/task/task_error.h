#pragma once

#include "service/common/http.h"

namespace service::task {

struct task_error {
    static inline constexpr service::common::app_error_def not_found{16701, "任务不存在", 404};
    static inline constexpr service::common::app_error_def not_failed{16702, "只能操作已失败同步标记",
        409};
    static inline constexpr service::common::app_error_def resource_changed{
        16703, "资源已删除或版本已变化，请刷新后重试", 409};
    static inline constexpr service::common::app_error_def cleanup_active{
        16704, "远程清理尚未完成，请先重新激活同步", 409};
};

}  // namespace service::task
