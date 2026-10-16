#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "service/common/domain_name.h"
#include "service/features/dns/record_model.h"
#include "service/features/dns/record_name_policy.h"
#include "service/features/dns_sync/model.h"

namespace service::dns_sync::detail {

struct record_changes final {
    zone_config_data writes_;
    zone_config_data removals_;
};

// A management request changes only the records edited in its revision.
// Unchanged rows never overwrite changes made in the provider's console.
inline record_changes plan_record_changes(const zone_config_data& previous, const zone_config_data& requested) {
    record_changes result;
    std::unordered_map<std::string, const zone_record_data*> before;
    std::unordered_set<std::string> retained;
    for (const auto& record : previous.records_) {
        before.emplace(record.id_, &record);
    }
    for (const auto& record : requested.records_) {
        retained.emplace(record.id_);
        const auto found = before.find(record.id_);
        if (found == before.end() || *found->second != record) {
            result.writes_.records_.push_back(record);
        }
    }
    for (const auto& record : previous.records_) {
        if (!retained.contains(record.id_)) {
            result.removals_.records_.push_back(record);
        }
    }
    return result;
}

inline service::dns::provider_record to_remote_record(const zone_record_data& record,
    const service::dns::record_name_policy& names,
    std::string_view domain) {
    return {
        .id_ = {},
        .type_ = record.type_,
        .name_ = names.remote_record_name(record.name_, domain),
        .content_ = record.content_,
        .ttl_ = record.ttl_,
        .priority_ = record.priority_,
        .proxied_ = record.proxied_,
        .line_code_ = record.line_code_,
    };
}

inline std::unordered_map<std::string, std::string>
collect_remote_ids_by_local_id(const zone_runtime_data& runtime) {
    std::unordered_map<std::string, std::string> result;
    result.reserve(runtime.record_states_.size());
    for (const auto& state : runtime.record_states_) {
        if (state.id_ && state.remote_record_id_) {
            result.emplace(*state.id_, *state.remote_record_id_);
        }
    }
    return result;
}

// The cluster owns A/AAAA records at its access hostname. Ordinary records at
// other names and types are never candidates for automatic removal.
inline std::unordered_set<std::string> obsolete_system_record_ids(const zone_runtime_data& previous,
    const zone_config_data& desired, const std::vector<service::dns::provider_record>& remote,
    const std::unordered_set<std::string>& cluster_hostnames,
    const service::dns::record_name_policy& names, std::string_view domain) {
    std::unordered_set<std::string> active_ids;
    for (const auto& record : desired.records_) {
        active_ids.emplace(record.id_);
    }
    std::unordered_set<std::string> retained;
    std::unordered_set<std::string> obsolete;
    for (const auto& state : previous.record_states_) {
        if (!state.managed_ || !state.id_ || !state.remote_record_id_) {
            continue;
        }
        (active_ids.contains(*state.id_) ? retained : obsolete).emplace(*state.remote_record_id_);
    }
    for (const auto& record : remote) {
        if ((record.type_ != "A" && record.type_ != "AAAA") ||
            !cluster_hostnames.contains(service::common::normalize_domain_name(names.remote_record_name(record.name_, domain)))) {
            continue;
        }
        const bool expected = std::ranges::any_of(desired.records_, [&](const auto& wanted) {
            const auto expected_name = names.remote_record_name(wanted.name_, domain);
            return wanted.type_ == record.type_ && wanted.content_ == record.content_ &&
                   wanted.line_code_ == record.line_code_ && service::common::normalize_domain_name(expected_name) == service::common::normalize_domain_name(names.remote_record_name(record.name_, domain));
        });
        if (!expected) {
            obsolete.emplace(record.id_);
        } else {
            retained.emplace(record.id_);
        }
    }
    for (const auto& id : retained) {
        obsolete.erase(id);
    }
    return obsolete;
}

// A desired record the sync leaves unpushed this attempt, with the reason shown on its record
// state; `name_` is relative to the hosted zone ("@", "www").
struct unsynced_record final {
    std::string id_;
    std::string name_;
    std::string type_;
    std::string error_;
};

// RFC 1034 §3.6.2: a CNAME owns its name alone. At the zone apex both providers keep MX and
// TXT beside it (Cloudflare through CNAME flattening, Aliyun by its documented "@" exception);
// A, AAAA and another CNAME never share the name, and below the apex no other record does.
// Aliyun applies the rule within one resolution line; Cloudflare only has the default line.
[[nodiscard]] inline bool coexists_with_cname(std::string_view type, bool zone_apex) {
    return zone_apex && (type == "MX" || type == "TXT");
}

// Website CNAMEs whose owner name already carries records they cannot share. FlexEdge never
// removes those records (they belong to the user, a cluster or a certificate challenge), so
// the website CNAME is held back and reported instead of making the provider reject the
// attempt over and over.
[[nodiscard]] inline std::vector<unsynced_record> find_blocked_website_records(
    const zone_config_data& desired, const std::unordered_set<std::string>& website_record_ids,
    const service::dns::record_name_policy& names, std::string_view domain) {
    std::vector<unsynced_record> result;
    for (const auto& record : desired.records_) {
        if (record.type_ != "CNAME" || !website_record_ids.contains(record.id_)) {
            continue;
        }
        const auto remote_name = names.remote_record_name(record.name_, domain);
        const auto owner = service::common::normalize_domain_name(remote_name);
        auto local_name = names.local_record_name(remote_name, domain);
        const bool zone_apex = local_name == "@";
        std::vector<std::string_view> blocking_types;
        for (const auto& other : desired.records_) {
            if (other.id_ == record.id_ || other.line_code_ != record.line_code_ ||
                service::common::normalize_domain_name(names.remote_record_name(other.name_, domain)) !=
                    owner ||
                coexists_with_cname(other.type_, zone_apex) ||
                (other.type_ == "CNAME" && service::common::normalize_domain_name(other.content_) ==
                                               service::common::normalize_domain_name(record.content_))) {
                continue;
            }
            if (std::ranges::find(blocking_types, other.type_) == blocking_types.end()) {
                blocking_types.push_back(other.type_);
            }
        }
        if (blocking_types.empty()) {
            continue;
        }
        std::string types;
        for (const auto type : blocking_types) {
            if (!types.empty()) {
                types += "、";
            }
            types += type;
        }
        auto error = "主机记录 " + local_name + " 已有 " + types +
                     " 记录，不能再添加 CNAME；请删除或修改这些记录，或把该网站域名改为外部解析";
        result.push_back({.id_ = record.id_,
            .name_ = std::move(local_name),
            .type_ = record.type_,
            .error_ = std::move(error)});
    }
    return result;
}

// The zone-level error of an attempt that left records unpushed: the count and the first reason.
[[nodiscard]] inline std::string unsynced_records_error(const std::vector<unsynced_record>& records) {
    if (records.empty()) {
        return {};
    }
    const auto& first = records.front();
    const auto subject = first.name_ + " " + first.type_ + "：" + first.error_;
    if (records.size() == 1) {
        return "1 条记录未同步：" + subject;
    }
    return std::to_string(records.size()) + " 条记录未同步，首条 " + subject;
}

}  // namespace service::dns_sync::detail
