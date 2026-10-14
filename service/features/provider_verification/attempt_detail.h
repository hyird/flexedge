#pragma once

#include <ruvia/web/model.h>

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <utility>
#include <vector>

#include "service/features/sync_runtime/attempt_record.h"

namespace service::provider_verification::detail {

RUVIA_MODEL(provider_zone_summary_dto, RUVIA_REQUIRED_FIELD(name, ruvia::string),
    RUVIA_REQUIRED_FIELD(status, ruvia::string));
RUVIA_MODEL(provider_attempt_detail_dto, RUVIA_REQUIRED_FIELD(kind, ruvia::string),
    RUVIA_REQUIRED_FIELD(truncated, ruvia::bool_value), RUVIA_REQUIRED_FIELD(provider, ruvia::string),
    RUVIA_REQUIRED_FIELD(provider_kind, ruvia::string), RUVIA_REQUIRED_FIELD(zone_count, ruvia::int64),
    RUVIA_REQUIRED_FIELD(zones, ruvia::array<provider_zone_summary_dto>),
    RUVIA_REQUIRED_FIELD(directory_url, ruvia::string), RUVIA_REQUIRED_FIELD(external_account, ruvia::string));

struct provider_zone_summary final {
    std::string name_;
    std::string status_;
};

// What one provider verification proved, filled only after each provider call
// succeeds. Never holds the account id, credentials, EAB key id or e-mail.
class provider_attempt final {
public:
    provider_attempt(std::string provider, std::string provider_kind)
        : provider_(std::move(provider)),
          provider_kind_(std::move(provider_kind)) {}

    void record_zone(std::string name, std::string status) {
        ++zone_count_;
        if (zones_.size() < service::sync_runtime::detail_list_items) {
            zones_.push_back({.name_ = std::move(name), .status_ = std::move(status)});
        }
    }

    // `external_account`: "reused", "fetched" or "not_required".
    void record_directory(std::string directory_url, std::string external_account) {
        directory_url_ = std::move(directory_url);
        external_account_ = std::move(external_account);
    }

    [[nodiscard]] std::string to_json(std::pmr::memory_resource* resource) const noexcept {
        return service::sync_runtime::serialize_detail(resource, [this](std::pmr::memory_resource* pool) {
            service::sync_runtime::detail_bounds bounds;
            provider_attempt_detail_dto output({.resource_ = pool});
            output.set<"kind">("provider");
            output.set<"provider">(bounds.text(provider_));
            output.set<"provider_kind">(bounds.text(provider_kind_));
            output.set<"zone_count">(static_cast<std::int64_t>(zone_count_));
            auto& zones = output.ensure<"zones">();
            for (const auto& zone : zones_) {
                provider_zone_summary_dto item({.resource_ = pool});
                item.set<"name">(bounds.text(zone.name_));
                item.set<"status">(bounds.text(zone.status_));
                if (!bounds.append(zones, std::move(item), pool)) {
                    break;
                }
            }
            if (zones.size() < zone_count_) {
                bounds.mark_truncated();
            }
            output.set<"directory_url">(bounds.text(directory_url_));
            output.set<"external_account">(bounds.text(external_account_));
            output.set<"truncated">(bounds.truncated());
            return output;
        });
    }

private:
    std::string provider_;
    std::string provider_kind_;
    std::size_t zone_count_{};
    std::vector<provider_zone_summary> zones_;
    std::string directory_url_;
    std::string external_account_;
};

}  // namespace service::provider_verification::detail
