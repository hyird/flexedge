#pragma once

#include <ruvia/web/model.h>

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "service/features/dns/record_model.h"
#include "service/features/dns/record_name_policy.h"
#include "service/features/dns_sync/reconciliation.h"
#include "service/features/sync_runtime/attempt_record.h"

namespace service::dns_sync::detail {

RUVIA_MODEL(dns_attempt_record_dto, RUVIA_REQUIRED_FIELD(name, ruvia::string),
    RUVIA_REQUIRED_FIELD(type, ruvia::string), RUVIA_REQUIRED_FIELD(line_code, ruvia::string),
    RUVIA_REQUIRED_FIELD(content, ruvia::string), RUVIA_REQUIRED_FIELD(ttl, ruvia::int64),
    RUVIA_REQUIRED_FIELD(priority, ruvia::int64, RUVIA_NULLABLE),
    RUVIA_REQUIRED_FIELD(proxied, ruvia::bool_value));
RUVIA_MODEL(dns_attempt_update_dto, RUVIA_REQUIRED_FIELD(before, dns_attempt_record_dto),
    RUVIA_REQUIRED_FIELD(after, dns_attempt_record_dto));
RUVIA_MODEL(dns_attempt_conflict_dto, RUVIA_REQUIRED_FIELD(name, ruvia::string),
    RUVIA_REQUIRED_FIELD(type, ruvia::string), RUVIA_REQUIRED_FIELD(line_code, ruvia::string),
    RUVIA_REQUIRED_FIELD(local_content, ruvia::string),
    RUVIA_REQUIRED_FIELD(remote_content, ruvia::string));
RUVIA_MODEL(dns_attempt_detail_dto, RUVIA_REQUIRED_FIELD(kind, ruvia::string),
    RUVIA_REQUIRED_FIELD(truncated, ruvia::bool_value),
    RUVIA_REQUIRED_FIELD(created, ruvia::array<dns_attempt_record_dto>),
    RUVIA_REQUIRED_FIELD(updated, ruvia::array<dns_attempt_update_dto>),
    RUVIA_REQUIRED_FIELD(deleted, ruvia::array<dns_attempt_record_dto>),
    RUVIA_REQUIRED_FIELD(created_count, ruvia::int64), RUVIA_REQUIRED_FIELD(updated_count, ruvia::int64),
    RUVIA_REQUIRED_FIELD(deleted_count, ruvia::int64), RUVIA_REQUIRED_FIELD(unchanged_count, ruvia::int64),
    RUVIA_REQUIRED_FIELD(conflicts, ruvia::array<dns_attempt_conflict_dto>),
    RUVIA_REQUIRED_FIELD(conflict_count, ruvia::int64));

// A provider record as the task history shows it: the name is relative to
// the hosted zone ("@", "www") whichever provider naming it came from.
struct attempt_record final {
    std::string name_;
    std::string type_;
    std::string line_code_;
    std::string content_;
    std::int64_t ttl_{};
    std::optional<std::int64_t> priority_;
    bool proxied_{};
};

struct attempt_record_update final {
    attempt_record before_;
    attempt_record after_;
};

[[nodiscard]] inline attempt_record to_attempt_record(const service::dns::provider_record& record,
    const service::dns::record_name_policy& names, std::string_view domain) {
    return {.name_ = names.local_record_name(record.name_, domain),
        .type_ = record.type_,
        .line_code_ = record.line_code_,
        .content_ = record.content_,
        .ttl_ = record.ttl_,
        .priority_ = record.priority_,
        .proxied_ = record.proxied_};
}

[[nodiscard]] inline attempt_record to_attempt_record(const managed_record& record) {
    return {.name_ = record.name_,
        .type_ = record.type_,
        .line_code_ = record.line_code_,
        .content_ = record.content_,
        .ttl_ = record.ttl_,
        .priority_ = record.priority_,
        .proxied_ = record.proxied_};
}

// Provider changes made by one DNS sync attempt, recorded as they happen so a
// failed attempt still reports the work already done. Lists keep at most the
// detail list limit; the counts stay exact.
class zone_sync_attempt final {
public:
    void record_created(attempt_record record) {
        ++created_count_;
        if (created_.size() < service::sync_runtime::detail_list_items) {
            created_.push_back(std::move(record));
        }
    }

    void record_updated(attempt_record before, attempt_record after) {
        ++updated_count_;
        if (updated_.size() < service::sync_runtime::detail_list_items) {
            updated_.push_back({.before_ = std::move(before), .after_ = std::move(after)});
        }
    }

    void record_deleted(attempt_record record) {
        ++deleted_count_;
        if (deleted_.size() < service::sync_runtime::detail_list_items) {
            deleted_.push_back(std::move(record));
        }
    }

    void record_unchanged() noexcept {
        ++unchanged_count_;
    }

    void record_conflicts(const std::vector<record_conflict>& conflicts) {
        conflict_count_ += conflicts.size();
        for (const auto& conflict : conflicts) {
            if (conflicts_.size() == service::sync_runtime::detail_list_items) {
                break;
            }
            conflicts_.push_back(conflict);
        }
    }

    [[nodiscard]] std::string to_json(std::pmr::memory_resource* resource) const noexcept {
        return service::sync_runtime::serialize_detail(resource, [this](std::pmr::memory_resource* pool) {
            service::sync_runtime::detail_bounds bounds;
            dns_attempt_detail_dto output({.resource_ = pool});
            output.set<"kind">("dns_sync");
            auto& created = output.ensure<"created">();
            for (const auto& record : created_) {
                if (!bounds.append(created, record_output(record, bounds, pool), pool)) {
                    break;
                }
            }
            auto& updated = output.ensure<"updated">();
            for (const auto& change : updated_) {
                dns_attempt_update_dto item({.resource_ = pool});
                item.set<"before">(record_output(change.before_, bounds, pool));
                item.set<"after">(record_output(change.after_, bounds, pool));
                if (!bounds.append(updated, std::move(item), pool)) {
                    break;
                }
            }
            auto& deleted = output.ensure<"deleted">();
            for (const auto& record : deleted_) {
                if (!bounds.append(deleted, record_output(record, bounds, pool), pool)) {
                    break;
                }
            }
            auto& conflicts = output.ensure<"conflicts">();
            for (const auto& conflict : conflicts_) {
                dns_attempt_conflict_dto item({.resource_ = pool});
                item.set<"name">(bounds.text(conflict.name_));
                item.set<"type">(bounds.text(conflict.type_));
                item.set<"line_code">(bounds.text(conflict.line_code_));
                item.set<"local_content">(bounds.text(conflict.local_content_));
                item.set<"remote_content">(bounds.text(conflict.remote_content_));
                if (!bounds.append(conflicts, std::move(item), pool)) {
                    break;
                }
            }
            if (created.size() < created_count_ || updated.size() < updated_count_ ||
                deleted.size() < deleted_count_ || conflicts.size() < conflict_count_) {
                bounds.mark_truncated();
            }
            output.set<"created_count">(static_cast<std::int64_t>(created_count_));
            output.set<"updated_count">(static_cast<std::int64_t>(updated_count_));
            output.set<"deleted_count">(static_cast<std::int64_t>(deleted_count_));
            output.set<"unchanged_count">(static_cast<std::int64_t>(unchanged_count_));
            output.set<"conflict_count">(static_cast<std::int64_t>(conflict_count_));
            output.set<"truncated">(bounds.truncated());
            return output;
        });
    }

private:
    static dns_attempt_record_dto record_output(const attempt_record& record,
        service::sync_runtime::detail_bounds& bounds, std::pmr::memory_resource* pool) {
        dns_attempt_record_dto output({.resource_ = pool});
        output.set<"name">(bounds.text(record.name_));
        output.set<"type">(bounds.text(record.type_));
        output.set<"line_code">(bounds.text(record.line_code_));
        output.set<"content">(bounds.text(record.content_));
        output.set<"ttl">(record.ttl_);
        if (record.priority_) {
            output.set<"priority">(*record.priority_);
        } else {
            output.set<"priority">(nullptr);
        }
        output.set<"proxied">(record.proxied_);
        return output;
    }

    std::vector<attempt_record> created_;
    std::vector<attempt_record_update> updated_;
    std::vector<attempt_record> deleted_;
    std::vector<record_conflict> conflicts_;
    std::size_t created_count_{};
    std::size_t updated_count_{};
    std::size_t deleted_count_{};
    std::size_t unchanged_count_{};
    std::size_t conflict_count_{};
};

}  // namespace service::dns_sync::detail
