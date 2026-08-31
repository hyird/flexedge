#pragma once

#include "service/common/http.h"

namespace service::task {

struct TaskError {
    static inline constexpr service::common::AppErrorDef NOT_FOUND{16701, "任务不存在", 404};
    static inline constexpr service::common::AppErrorDef NOT_FAILED{16702, "只能操作已失败同步标记",
                                                                    409};
    static inline constexpr service::common::AppErrorDef RESOURCE_CHANGED{
        16703, "资源已删除或版本已变化，请刷新后重试", 409};
    static inline constexpr service::common::AppErrorDef CLEANUP_ACTIVE{
        16704, "远程清理尚未完成，请先重新激活同步", 409};
};

} // namespace service::task
