#pragma once

#include "service/common/http.h"

namespace service::task {

struct task_error {
    static inline constexpr service::common::app_error_def not_found{16701, "任务不存在", 404};
    static inline constexpr service::common::app_error_def subtask_not_found{16705, "子任务不存在", 404};
    static inline constexpr service::common::app_error_def attempt_not_found{16706, "执行记录不存在", 404};
};

}  // namespace service::task
