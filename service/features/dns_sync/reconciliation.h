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

inline constexpr std::size_t max_reconciliation_records{10'000};

inline bool supports_managed_record_type(std::string_view type) {
    return type == "A" || type == "AAAA" || type == "CNAME" || type == "TXT" || type == "MX";
}

struct managed_record final {
    std::string id_;
    std::string type_;
    std::string name_;
    std::string content_;
    std::int64_t ttl_;
    std::optional<std::int64_t> priority_;
    bool proxied_;
    std::string line_code_;
    std::string remote_id_{};
};

inline std::string record_identity(const service::dns::provider_record& record) {
    return record.type_ + "\x1f" + service::common::normalize_domain_name(record.name_) + "\x1f" +
           record.line_code_;
}

inline service::dns::provider_record to_remote_record(const managed_record& record,
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

inline bool record_matches(const managed_record& local, const service::dns::provider_record& remote,
    const service::dns::record_name_policy& names, std::string_view domain) {
    const auto expected = to_remote_record(local, names, domain);
    return expected.type_ == remote.type_ && expected.content_ == remote.content_ &&
           expected.ttl_ == remote.ttl_ &&
           (expected.type_ != "MX" || expected.priority_ == remote.priority_) &&
           expected.proxied_ == remote.proxied_ && expected.line_code_ == remote.line_code_ &&
           service::common::normalize_domain_name(expected.name_) ==
               service::common::normalize_domain_name(remote.name_);
}

inline void apply_remote_record(managed_record& target, const service::dns::provider_record& source,
    const service::dns::record_name_policy& names,
    std::string_view domain) {
    target.type_ = source.type_;
    target.name_ = names.local_record_name(source.name_, domain);
    target.content_ = source.content_;
    target.ttl_ = source.ttl_;
    target.priority_ = source.priority_;
    target.proxied_ = source.proxied_;
    target.line_code_ = source.line_code_;
}

inline void validate_remote_record(const service::dns::provider_record& record,
    const std::unordered_set<std::string>& enabled_lines) {
    if (!supports_managed_record_type(record.type_)) {
        throw std::runtime_error("远程 DNS 存在当前不支持自动托管的记录类型：" + record.type_);
    }
    if (record.id_.empty() || record.name_.empty() || record.content_.empty() || record.ttl_ < 1 ||
        record.ttl_ > 86400 ||
        (record.priority_ && (*record.priority_ < 0 || *record.priority_ > 65535)) ||
        !enabled_lines.contains(record.line_code_)) {
        throw std::runtime_error("远程 DNS 记录不符合本地托管配置要求");
    }
}

struct record_conflict final {
    std::string id_;
    std::string type_;
    std::string name_;
    std::string line_code_;
    std::string local_content_;
    std::string remote_content_;
};

struct managed_record_set final {
    std::vector<managed_record> records_;
    std::unordered_set<std::string> ids_;
};

struct remote_merge_plan final {
    std::vector<managed_record> records_;
    std::unordered_map<std::string, std::string> remote_ids_by_local_id_;
    std::vector<record_conflict> conflicts_;
    bool changed_{};
};

inline std::unordered_map<std::string, const service::dns::provider_record*>
index_remote_records(const std::vector<service::dns::provider_record>& remote_records,
    const std::vector<service::dns::provider_line>& lines) {
    if (remote_records.size() > max_reconciliation_records) {
        throw std::runtime_error("远程 DNS 记录超过单个域名可导入上限");
    }

    std::unordered_set<std::string> enabled_lines;
    enabled_lines.reserve(lines.size());
    for (const auto& line : lines) {
        if (!line.code_.empty()) {
            enabled_lines.emplace(line.code_);
        }
    }

    std::unordered_map<std::string, const service::dns::provider_record*> result;
    result.reserve(remote_records.size());
    for (const auto& record : remote_records) {
        validate_remote_record(record, enabled_lines);
        if (!result.emplace(record.id_, &record).second) {
            throw std::runtime_error("远程 DNS 记录 ID 重复");
        }
    }
    return result;
}

inline managed_record to_managed_record(const zone_record_data& record) {
    return {
        .id_ = record.id_,
        .type_ = record.type_,
        .name_ = record.name_,
        .content_ = record.content_,
        .ttl_ = record.ttl_,
        .priority_ = record.priority_,
        .proxied_ = record.proxied_,
        .line_code_ = record.line_code_,
    };
}

inline managed_record_set build_managed_records(const zone_config_data& config) {
    managed_record_set result;
    result.records_.reserve(config.records_.size());
    result.ids_.reserve(config.records_.size());
    for (const auto& record : config.records_) {
        auto managed = to_managed_record(record);
        if (!result.ids_.emplace(managed.id_).second) {
            throw std::runtime_error("DNS 聚合配置中的记录 ID 重复");
        }
        result.records_.push_back(std::move(managed));
    }
    return result;
}

inline std::unordered_set<std::string>
collect_desired_config_record_ids(const zone_config_data& desired_config) {
    std::unordered_set<std::string> result;
    result.reserve(desired_config.records_.size());
    for (const auto& record : desired_config.records_) {
        result.emplace(record.id_);
    }
    return result;
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

inline record_conflict make_record_conflict(const managed_record& local,
    std::string_view remote_content) {
    return {
        .id_ = local.id_,
        .type_ = local.type_,
        .name_ = local.name_,
        .line_code_ = local.line_code_,
        .local_content_ = local.content_,
        .remote_content_ = std::string(remote_content),
    };
}

inline bool requires_manual_resolution(const managed_record& local,
    const service::dns::provider_record& remote,
    const service::dns::record_name_policy& names,
    std::string_view domain) {
    const auto expected = to_remote_record(local, names, domain);
    return expected.type_ != remote.type_ ||
           service::common::normalize_domain_name(expected.name_) !=
               service::common::normalize_domain_name(remote.name_) ||
           expected.content_ != remote.content_;
}

inline const service::dns::provider_record*
find_unclaimed_remote_record(const managed_record& local,
    const std::unordered_set<std::string>& claimed_remote_ids,
    const std::vector<service::dns::provider_record>& remote_records,
    const service::dns::record_name_policy& names, std::string_view domain) {
    const auto identity = record_identity(to_remote_record(local, names, domain));
    const service::dns::provider_record* candidate = nullptr;
    for (const auto& remote : remote_records) {
        if (claimed_remote_ids.contains(remote.id_) || record_identity(remote) != identity) {
            continue;
        }
        if (candidate) {
            throw std::runtime_error("远程 DNS 存在无法安全合并的同名记录");
        }
        candidate = &remote;
    }
    return candidate;
}

inline std::vector<record_conflict> collect_sync_conflicts(
    const std::vector<managed_record>& local_records,
    const std::unordered_map<std::string, std::string>& remote_ids_by_local_id,
    const std::unordered_set<std::string>& config_ids,
    const std::unordered_map<std::string, const service::dns::provider_record*>& remote_by_id,
    const std::vector<service::dns::provider_record>& remote_records,
    const service::dns::record_name_policy& names, std::string_view domain) {
    std::unordered_set<std::string> claimed_remote_ids;
    claimed_remote_ids.reserve(remote_ids_by_local_id.size());
    for (const auto& [id, remote_id] : remote_ids_by_local_id) {
        if (config_ids.contains(id) && remote_by_id.contains(remote_id)) {
            claimed_remote_ids.emplace(remote_id);
        }
    }

    std::vector<record_conflict> conflicts;
    for (const auto& local : local_records) {
        const auto known = remote_ids_by_local_id.find(local.id_);
        if (known != remote_ids_by_local_id.end()) {
            const auto remote = remote_by_id.find(known->second);
            if (remote == remote_by_id.end()) {
                conflicts.push_back(make_record_conflict(local, "（远端已删除）"));
            } else if (requires_manual_resolution(local, *remote->second, names, domain)) {
                conflicts.push_back(make_record_conflict(local, remote->second->content_));
            }
            continue;
        }

        const auto* candidate =
            find_unclaimed_remote_record(local, claimed_remote_ids, remote_records, names, domain);
        if (candidate && requires_manual_resolution(local, *candidate, names, domain)) {
            conflicts.push_back(make_record_conflict(local, candidate->content_));
        }
    }
    return conflicts;
}

inline std::unordered_set<std::string> claim_known_remote_ids(
    std::unordered_map<std::string, std::string>& remote_ids_by_local_id,
    const std::unordered_set<std::string>& config_ids,
    const std::unordered_set<std::string>& desired_config_ids,
    const std::unordered_map<std::string, const service::dns::provider_record*>& remote_by_id,
    bool remote_preferred, bool& changed) {
    std::unordered_set<std::string> claimed_remote_ids;
    claimed_remote_ids.reserve(remote_ids_by_local_id.size());
    for (auto it = remote_ids_by_local_id.begin(); it != remote_ids_by_local_id.end();) {
        const auto remote = remote_by_id.find(it->second);
        if (remote == remote_by_id.end()) {
            ++it;
            continue;
        }
        if (!config_ids.contains(it->first) && !desired_config_ids.contains(it->first) &&
            remote_preferred) {
            it = remote_ids_by_local_id.erase(it);
            changed = true;
            continue;
        }
        claimed_remote_ids.emplace(it->second);
        ++it;
    }
    return claimed_remote_ids;
}

inline bool merge_managed_record(
    managed_record& local, std::unordered_map<std::string, std::string>& remote_ids_by_local_id,
    std::unordered_set<std::string>& claimed_remote_ids,
    const std::unordered_map<std::string, const service::dns::provider_record*>& remote_by_id,
    const std::vector<service::dns::provider_record>& remote_records,
    const service::dns::record_name_policy& names, std::string_view domain, bool remote_preferred) {
    const auto known = remote_ids_by_local_id.find(local.id_);
    if (known != remote_ids_by_local_id.end()) {
        const auto remote = remote_by_id.find(known->second);
        if (remote == remote_by_id.end()) {
            if (!remote_preferred) {
                return false;
            }
            local.id_.clear();
            remote_ids_by_local_id.erase(known);
            return true;
        }
        if (remote_preferred && !record_matches(local, *remote->second, names, domain)) {
            apply_remote_record(local, *remote->second, names, domain);
            return true;
        }
        return false;
    }

    const auto* candidate =
        find_unclaimed_remote_record(local, claimed_remote_ids, remote_records, names, domain);
    if (!candidate) {
        return false;
    }
    remote_ids_by_local_id.emplace(local.id_, candidate->id_);
    claimed_remote_ids.emplace(candidate->id_);
    if (remote_preferred && !record_matches(local, *candidate, names, domain)) {
        apply_remote_record(local, *candidate, names, domain);
    }
    return true;
}

inline void append_unclaimed_remote_records(
    std::vector<managed_record>& merged, const std::unordered_set<std::string>& claimed_remote_ids,
    const std::vector<service::dns::provider_record>& remote_records,
    const service::dns::record_name_policy& names, std::string_view domain, bool& changed) {
    std::erase_if(merged, [](const auto& record) { return record.id_.empty(); });
    for (const auto& remote : remote_records) {
        if (claimed_remote_ids.contains(remote.id_)) {
            continue;
        }
        merged.push_back(managed_record{
            .id_ = {},
            .type_ = remote.type_,
            .name_ = names.local_record_name(remote.name_, domain),
            .content_ = remote.content_,
            .ttl_ = remote.ttl_,
            .priority_ = remote.priority_,
            .proxied_ = remote.proxied_,
            .line_code_ = remote.line_code_,
            .remote_id_ = remote.id_,
        });
        changed = true;
    }
}

inline remote_merge_plan
plan_remote_merge(std::string_view operation, const zone_config_data& config,
    const zone_runtime_data& runtime, const zone_config_data& desired_config,
    std::string_view domain, const service::dns::record_name_policy& names,
    const std::vector<service::dns::provider_record>& remote_records,
    const std::vector<service::dns::provider_line>& lines) {
    const auto remote_by_id = index_remote_records(remote_records, lines);
    auto managed = build_managed_records(config);
    const auto desired_config_ids = collect_desired_config_record_ids(desired_config);
    auto remote_ids_by_local_id = collect_remote_ids_by_local_id(runtime);

    remote_merge_plan result;
    if (operation == "sync") {
        result.conflicts_ = collect_sync_conflicts(managed.records_, remote_ids_by_local_id, managed.ids_,
            remote_by_id, remote_records, names, domain);
        if (!result.conflicts_.empty()) {
            return result;
        }
    }

    result.changed_ = !runtime.records_imported_;
    const bool remote_preferred = operation == "sync_remote";
    auto claimed_remote_ids = claim_known_remote_ids(remote_ids_by_local_id, managed.ids_, desired_config_ids,
        remote_by_id, remote_preferred, result.changed_);
    for (auto& local : managed.records_) {
        if (merge_managed_record(local, remote_ids_by_local_id, claimed_remote_ids, remote_by_id,
                remote_records, names, domain, remote_preferred)) {
            result.changed_ = true;
        }
    }
    append_unclaimed_remote_records(managed.records_, claimed_remote_ids, remote_records, names, domain,
        result.changed_);
    result.records_ = std::move(managed.records_);
    result.remote_ids_by_local_id_ = std::move(remote_ids_by_local_id);
    return result;
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
