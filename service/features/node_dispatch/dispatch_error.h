#pragma once

#include "service/common/http.h"

namespace service::node_dispatch {

struct dispatch_error {
    // A website the release would carry breaks the node protocol rules; nodes would reject
    // the whole release, so it is not published.
    static inline constexpr service::common::app_error_def website_protocol_invalid{
        16617, "网站配置不符合节点协议，无法发布", 422};
};

}  // namespace service::node_dispatch
