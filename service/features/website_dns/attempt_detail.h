#pragma once

#include <ruvia/web/model.h>

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <utility>
#include <vector>

#include "service/features/sync_runtime/attempt_record.h"

namespace service::website_dns {

RUVIA_MODEL(website_domain_check_dto, RUVIA_REQUIRED_FIELD(hostname, ruvia::string),
    RUVIA_REQUIRED_FIELD(query_name, ruvia::string), RUVIA_REQUIRED_FIELD(expected, ruvia::string),
    RUVIA_REQUIRED_FIELD(resolved, ruvia::array<ruvia::string>), RUVIA_REQUIRED_FIELD(status, ruvia::string),
    RUVIA_REQUIRED_FIELD(error, ruvia::string));
RUVIA_MODEL(website_domains_detail_dto, RUVIA_REQUIRED_FIELD(kind, ruvia::string),
    RUVIA_REQUIRED_FIELD(truncated, ruvia::bool_value),
    RUVIA_REQUIRED_FIELD(expected_target, ruvia::string), RUVIA_REQUIRED_FIELD(domain_count, ruvia::int64),
    RUVIA_REQUIRED_FIELD(checked_count, ruvia::int64),
    RUVIA_REQUIRED_FIELD(domains, ruvia::array<website_domain_check_dto>));

// CNAME answers kept per checked hostname.
inline constexpr std::size_t max_resolved_targets{8};

struct website_domain_check final {
    std::string hostname_;
    std::string query_name_;
    std::vector<std::string> resolved_;
    std::string status_;
    std::string error_;
};

// Per-hostname results of one website DNS check, recorded as each probe
// finishes so a failed attempt keeps the hostnames already checked.
class website_domain_attempt final {
public:
    void start(std::string expected_target, std::size_t domain_count) {
        expected_target_ = std::move(expected_target);
        domain_count_ = domain_count;
    }

    void record_check(website_domain_check check) {
        ++checked_count_;
        if (check.status_ != "verified") {
            ++failed_count_;
            if (first_error_.empty()) {
                first_error_ = check.hostname_ + "：" + check.error_;
            }
        }
        if (checks_.size() < service::sync_runtime::detail_list_items) {
            checks_.push_back(std::move(check));
        }
    }

    [[nodiscard]] std::string error() const {
        if (failed_count_ == 0) {
            return {};
        }
        bool truncated = false;
        return service::sync_runtime::bounded_detail_text(
            std::to_string(failed_count_) + " 个域名解析异常，首个 " + first_error_, truncated, 1000);
    }

    [[nodiscard]] std::string to_json(std::pmr::memory_resource* resource) const noexcept {
        return service::sync_runtime::serialize_detail(resource, [this](std::pmr::memory_resource* pool) {
            service::sync_runtime::detail_bounds bounds;
            website_domains_detail_dto output({.resource_ = pool});
            output.set<"kind">("website_domains");
            output.set<"expected_target">(bounds.text(expected_target_));
            output.set<"domain_count">(static_cast<std::int64_t>(domain_count_));
            output.set<"checked_count">(static_cast<std::int64_t>(checked_count_));
            auto& domains = output.ensure<"domains">();
            for (const auto& check : checks_) {
                website_domain_check_dto item({.resource_ = pool});
                item.set<"hostname">(bounds.text(check.hostname_));
                item.set<"query_name">(bounds.text(check.query_name_));
                item.set<"expected">(bounds.text(expected_target_));
                auto& resolved = item.ensure<"resolved">();
                for (const auto& target : check.resolved_) {
                    if (resolved.size() == max_resolved_targets) {
                        bounds.mark_truncated();
                        break;
                    }
                    resolved.emplace_back(bounds.text(target), ruvia::model_options{.resource_ = pool});
                }
                item.set<"status">(bounds.text(check.status_));
                item.set<"error">(bounds.text(check.error_));
                if (!bounds.append(domains, std::move(item), pool)) {
                    break;
                }
            }
            if (domains.size() < checked_count_) {
                bounds.mark_truncated();
            }
            output.set<"truncated">(bounds.truncated());
            return output;
        });
    }

private:
    std::string expected_target_;
    std::size_t domain_count_{};
    std::size_t checked_count_{};
    std::size_t failed_count_{};
    std::string first_error_;
    std::vector<website_domain_check> checks_;
};

}  // namespace service::website_dns
