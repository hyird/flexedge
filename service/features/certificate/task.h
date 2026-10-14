#pragma once

#include <cstdint>
#include <string>

namespace service::certificate_issuance::worker_detail {

struct certificate_task final {
    std::string id_;
    std::string tenant_id_;
    std::string certificate_id_;
    std::int64_t version_;
    std::int64_t failures_;
};

}  // namespace service::certificate_issuance::worker_detail
