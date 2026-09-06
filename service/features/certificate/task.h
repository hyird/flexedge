#pragma once

#include <cstdint>
#include <string>

namespace service::certificate_issuance::worker_detail {

struct CertificateTask final {
    std::string id;
    std::string tenantId;
    std::string certificateId;
    std::int64_t version;
    std::int64_t failures;
};

} // namespace service::certificate_issuance::worker_detail
