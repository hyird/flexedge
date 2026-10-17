#pragma once

#include <algorithm>
#include <array>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <openssl/evp.h>

#include "service/features/dns/record_model.h"
#include "service/features/dns/record_name_policy.h"
#include "service/features/dns_sync/model.h"

namespace service::dns_sync {

inline bool editable_record_type(std::string_view type) {
    return type == "A" || type == "AAAA" || type == "CNAME" || type == "TXT" || type == "MX";
}

// Provider ids need not be UUIDs. A stable, zone-scoped UUID keeps the management
// contract without importing provider records as desired configuration.
inline std::string snapshot_record_id(std::string_view zone_id, std::string_view remote_id) {
    const auto identity = std::string(zone_id) + ":" + std::string(remote_id);
    std::array<unsigned char, 32> digest{};
    if (EVP_Q_digest(nullptr, "SHA256", nullptr, identity.data(), identity.size(), digest.data(), nullptr) != 1) {
        throw std::runtime_error("DNS 记录标识生成失败");
    }
    digest[6] = static_cast<unsigned char>((digest[6] & 0x0f) | 0x80);
    digest[8] = static_cast<unsigned char>((digest[8] & 0x3f) | 0x80);
    constexpr std::string_view hex{"0123456789abcdef"};
    std::string result;
    for (std::size_t index = 0; index < 16; ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) {
            result += '-';
        }
        result += hex[digest[index] >> 4];
        result += hex[digest[index] & 0x0f];
    }
    return result;
}

struct zone_snapshot final {
    // Ordinary records are an observation, never background write intent.
    zone_config_data records_;
    zone_runtime_data runtime_;
};

inline zone_snapshot capture_zone_snapshot(std::string_view zone_id, std::string_view domain,
    const service::dns::record_name_policy& names,
    const std::vector<service::dns::provider_record>& remote_records,
    const std::vector<service::dns::provider_line>& lines, const zone_runtime_data& previous,
    std::span<const zone_record_runtime_data> managed_states, std::int64_t revision) {
    if (remote_records.size() > 10'000) {
        throw std::runtime_error("远程 DNS 记录超过单个域名可管理上限");
    }
    zone_snapshot result;
    result.runtime_.challenge_records_ = previous.challenge_records_;
    result.runtime_.record_states_.assign(managed_states.begin(), managed_states.end());
    std::unordered_set<std::string> managed_remote_ids;
    for (const auto& state : managed_states) {
        if (state.remote_record_id_) {
            managed_remote_ids.emplace(*state.remote_record_id_);
        }
    }
    std::unordered_map<std::string, std::string> previous_ids;
    for (const auto& state : previous.record_states_) {
        if (!state.managed_ && state.id_ && state.remote_record_id_) {
            previous_ids.emplace(*state.remote_record_id_, *state.id_);
        }
    }
    std::unordered_set<std::string> remote_ids;
    for (const auto& remote : remote_records) {
        if (remote.id_.empty() || !remote_ids.emplace(remote.id_).second) {
            throw std::runtime_error("远程 DNS 记录 ID 为空或重复");
        }
        // Unsupported record types stay with the provider; they never prevent
        // system records from being synchronized or become cleanup candidates.
        if (managed_remote_ids.contains(remote.id_) || !editable_record_type(remote.type_)) {
            continue;
        }
        const auto known = previous_ids.find(remote.id_);
        const auto id = known == previous_ids.end() ? snapshot_record_id(zone_id, remote.id_) : known->second;
        result.records_.records_.push_back({.id_ = id,
            .type_ = remote.type_,
            .name_ = names.local_record_name(remote.name_, domain),
            .content_ = remote.content_,
            .ttl_ = remote.ttl_,
            .priority_ = remote.priority_,
            .proxied_ = remote.proxied_,
            .line_code_ = remote.line_code_});
        result.runtime_.record_states_.push_back({.id_ = id,
            .remote_record_id_ = remote.id_,
            .sync_status_ = "synced",
            .synced_revision_ = revision,
            .last_error_ = {},
            .managed_ = false});
    }
    for (const auto& line : lines) {
        result.runtime_.lines_.push_back({.code_ = line.code_, .name_ = line.name_, .display_name_ = line.display_name_, .status_ = "enabled"});
    }
    std::ranges::sort(result.records_.records_, {}, &zone_record_data::id_);
    std::ranges::sort(result.runtime_.record_states_, {}, &zone_record_runtime_data::id_);
    return result;
}

}  // namespace service::dns_sync
