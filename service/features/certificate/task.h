#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace service::certificate_issuance::worker_detail {

struct certificate_task final {
    std::string id_;
    std::string tenant_id_;
    std::string certificate_id_;
    std::int64_t version_;
    std::int64_t failures_;
    // Marker operation: "issue" or "renew".
    std::string operation_;
    // Monotonic instant the claim acquired the lease: the attempt start.
    std::chrono::steady_clock::time_point claimed_at_{};
};

}  // namespace service::certificate_issuance::worker_detail
