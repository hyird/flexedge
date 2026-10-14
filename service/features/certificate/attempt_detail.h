#pragma once

#include <ruvia/web/model.h>

#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "service/features/sync_runtime/attempt_record.h"

namespace service::certificate_issuance {

// Issuance stages in execution order; a failed attempt reports the stage it
// stopped in, a successful one `completed`.
enum class certificate_stage {
    prepare,
    account,
    order,
    authorization,
    dns_challenge,
    validation,
    cleanup,
    finalize,
    download,
    persist,
    completed,
};

[[nodiscard]] inline constexpr std::string_view certificate_stage_name(certificate_stage stage) noexcept {
    switch (stage) {
        case certificate_stage::prepare:
            return "prepare";
        case certificate_stage::account:
            return "account";
        case certificate_stage::order:
            return "order";
        case certificate_stage::authorization:
            return "authorization";
        case certificate_stage::dns_challenge:
            return "dns_challenge";
        case certificate_stage::validation:
            return "validation";
        case certificate_stage::cleanup:
            return "cleanup";
        case certificate_stage::finalize:
            return "finalize";
        case certificate_stage::download:
            return "download";
        case certificate_stage::persist:
            return "persist";
        case certificate_stage::completed:
            return "completed";
    }
    return "prepare";
}

RUVIA_MODEL(certificate_attempt_detail_dto, RUVIA_REQUIRED_FIELD(kind, ruvia::string),
    RUVIA_REQUIRED_FIELD(truncated, ruvia::bool_value), RUVIA_REQUIRED_FIELD(operation, ruvia::string),
    RUVIA_REQUIRED_FIELD(provider, ruvia::string), RUVIA_REQUIRED_FIELD(domains, ruvia::array<ruvia::string>),
    RUVIA_REQUIRED_FIELD(stage, ruvia::string), RUVIA_REQUIRED_FIELD(issuer, ruvia::string),
    RUVIA_REQUIRED_FIELD(not_before, ruvia::string), RUVIA_REQUIRED_FIELD(not_after, ruvia::string),
    RUVIA_REQUIRED_FIELD(serial_number, ruvia::string),
    RUVIA_REQUIRED_FIELD(fingerprint_sha256, ruvia::string));

// Public identity of an issued certificate; never the key or the chain.
struct issued_certificate_summary final {
    std::string issuer_;
    std::string not_before_;
    std::string not_after_;
    std::string serial_number_;
    std::string fingerprint_sha256_;
};

// Progress of one issue/renew attempt, owned by the worker for the whole
// attempt so a failure still reports the stage it reached.
struct certificate_attempt final {
    std::string operation_;
    std::string provider_;
    std::vector<std::string> domains_;
    certificate_stage stage_{certificate_stage::prepare};
    std::optional<issued_certificate_summary> issued_;

    [[nodiscard]] std::string to_json(std::pmr::memory_resource* resource) const noexcept {
        return service::sync_runtime::serialize_detail(resource, [this](std::pmr::memory_resource* pool) {
            service::sync_runtime::detail_bounds bounds;
            certificate_attempt_detail_dto output({.resource_ = pool});
            output.set<"kind">("certificate");
            output.set<"operation">(bounds.text(operation_));
            output.set<"provider">(bounds.text(provider_));
            auto& domains = output.ensure<"domains">();
            for (const auto& domain : domains_) {
                if (domains.size() == service::sync_runtime::detail_list_items) {
                    bounds.mark_truncated();
                    break;
                }
                domains.emplace_back(bounds.text(domain), ruvia::model_options{.resource_ = pool});
            }
            output.set<"stage">(certificate_stage_name(stage_));
            const issued_certificate_summary empty{};
            const auto& issued = issued_ && stage_ == certificate_stage::completed ? *issued_ : empty;
            output.set<"issuer">(bounds.text(issued.issuer_));
            output.set<"not_before">(bounds.text(issued.not_before_));
            output.set<"not_after">(bounds.text(issued.not_after_));
            output.set<"serial_number">(bounds.text(issued.serial_number_));
            output.set<"fingerprint_sha256">(bounds.text(issued.fingerprint_sha256_));
            output.set<"truncated">(bounds.truncated());
            return output;
        });
    }
};

}  // namespace service::certificate_issuance
