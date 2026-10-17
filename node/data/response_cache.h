#pragma once
#include <atomic>
#include <charconv>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/cache_policy.h"
#include "common/cache_url.h"
#include "node/data/buffered_proxy_response.h"
#include "node/data/cache_capacity.h"
#include "node/proto/edge_control.pb.h"

namespace flexedge::node {
using cache_headers = std::vector<std::pair<std::string, std::string>>;

inline std::string cache_header(const cache_headers& headers, std::string_view name) {
    std::string result;
    for (const auto& [key, value] : headers) {
        if (flexedge::route_policy::ascii_equal(key, name)) {
            if (!result.empty()) {
                result.append(",");
            }
            result.append(value);
        }
    }
    return result;
}
inline std::vector<std::string> cache_tokens(std::string_view input) {
    std::vector<std::string> result;
    while (!input.empty()) {
        const auto comma = input.find(',');
        auto token = std::string(flexedge::route_policy::trim_ows(input.substr(0, comma)));
        std::ranges::transform(token, token.begin(), [](unsigned char c) { return flexedge::route_policy::lower_ascii(c); });
        if (!token.empty()) {
            result.push_back(std::move(token));
        }
        if (comma == std::string_view::npos) {
            break;
        }
        input.remove_prefix(comma + 1);
    }
    return result;
}
inline bool cache_has_directive(std::string_view input, std::string_view name) {
    for (const auto& token : cache_tokens(input)) {
        if (flexedge::route_policy::trim_ows(std::string_view(token).substr(0, token.find('='))) == name) {
            return true;
        }
    }
    return false;
}
inline std::optional<std::int64_t> cache_directive_seconds(std::string_view input, std::string_view name) {
    std::optional<std::int64_t> result;
    for (const auto& token : cache_tokens(input)) {
        const auto equal = token.find('=');
        if (equal == std::string::npos || flexedge::route_policy::trim_ows(std::string_view(token).substr(0, equal)) != name) {
            continue;
        }
        auto value = flexedge::route_policy::trim_ows(std::string_view(token).substr(equal + 1));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }
        std::int64_t seconds{};
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), seconds);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || seconds < 0 || result) {
            return 0;
        }
        result = (std::min)(seconds, std::int64_t{31536000});
    }
    return result;
}
inline std::optional<std::time_t> cache_http_time(std::string_view text) {
    std::tm value{};
    std::istringstream stream{std::string(text)};
    stream.imbue(std::locale::classic());
    stream >> std::get_time(&value, "%a, %d %b %Y %H:%M:%S GMT");
    if (stream.fail()) {
        return {};
    }
#ifdef _WIN32
    return _mkgmtime(&value);
#else
    return timegm(&value);
#endif
}
inline std::string cache_http_date(std::time_t timestamp) {
    std::tm value{};
#ifdef _WIN32
    gmtime_s(&value, &timestamp);
#else
    gmtime_r(&timestamp, &value);
#endif
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::put_time(&value, "%a, %d %b %Y %H:%M:%S GMT");
    return stream.str();
}
inline bool cache_wildcard(std::string_view value, std::string_view pattern) {
    std::size_t at{}, next{}, star = std::string_view::npos, retry{};
    while (at < value.size()) {
        if (next < pattern.size() && (pattern[next] == '?' || pattern[next] == value[at])) {
            ++at;
            ++next;
        } else if (next < pattern.size() && pattern[next] == '*') {
            star = next++;
            retry = at;
        } else if (star != std::string_view::npos) {
            next = star + 1;
            at = ++retry;
        } else {
            return false;
        }
    }
    while (next < pattern.size() && pattern[next] == '*') {
        ++next;
    }
    return next == pattern.size();
}

// Published objects and every value reachable from them are immutable.
// File handles and response cursors belong to the reading worker.
struct cache_file final {
    explicit cache_file(std::filesystem::path path)
        : path_(std::move(path)) {}
    ~cache_file() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }
    const std::filesystem::path path_;
};
struct cached_response final {
    std::string website_id_{};
    std::string policy_id_{};
    std::string url_{};
    cache_headers headers_{};
    cache_headers vary_{};
    std::shared_ptr<const std::string> body_{};
    std::shared_ptr<const cache_file> file_{};
    std::uint32_t status_{200};
    std::size_t body_bytes_{};
    std::size_t bytes_{};
    std::uint64_t sequence_{};
    std::int64_t initial_age_{};
    std::int64_t browser_ttl_seconds_{};
    bool ignore_query_{};
    std::chrono::steady_clock::time_point stored_at_{};
    std::chrono::steady_clock::time_point expires_at_{};
};
struct cache_fill_claim final {
    std::uint64_t id_{};
    std::chrono::steady_clock::time_point expires_at_{};
};
struct response_cache_snapshot final {
    std::unordered_map<std::string, std::vector<std::shared_ptr<const cached_response>>> entries_{};
    std::unordered_map<std::string, std::uint64_t> epochs_{};
    std::unordered_map<std::string, cache_fill_claim> filling_{};
    std::unordered_map<std::string, std::shared_ptr<const v2::CacheResult>> commands_{};
    std::uint64_t sequence_{};
};

// This object only publishes roots. It exposes no shared mutable cache objects,
// counters, handles or callbacks; writers merge into a private copy and CAS it.
class response_cache_publication final {
public:
    explicit response_cache_publication(std::filesystem::path directory = "cache")
        : directory_(std::filesystem::absolute(std::move(directory))),
          current_(std::make_shared<const response_cache_snapshot>()) {
        // This directory is owned exclusively by the node. A restart starts with a
        // cold index, so remove only our recognizable orphan body/temporary files.
        std::filesystem::create_directories(directory_);
        for (auto parent = directory_; !parent.empty() && parent != parent.root_path(); parent = parent.parent_path()) {
            if (std::filesystem::is_symlink(parent)) {
                throw std::runtime_error("cache directory cannot contain symlinks");
            }
        }
        for (const auto& entry : std::filesystem::recursive_directory_iterator(directory_)) {
            if (!entry.is_regular_file() || entry.is_symlink()) {
                continue;
            }
            const auto name = entry.path().stem().string(), extension = entry.path().extension().string();
            if (name.size() == 32 && (extension == ".body" || extension == ".tmp") &&
                std::ranges::all_of(name, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); })) {
                std::error_code ignored;
                std::filesystem::remove(entry.path(), ignored);
            }
        }
    }
    std::shared_ptr<const response_cache_snapshot> snapshot() const {
        return current_.load(std::memory_order_acquire);
    }
    bool publish(std::shared_ptr<const response_cache_snapshot>& expected,
        std::shared_ptr<const response_cache_snapshot> replacement) {
        return current_.compare_exchange_weak(expected, std::move(replacement), std::memory_order_release, std::memory_order_acquire);
    }
    const std::filesystem::path& directory() const {
        return directory_;
    }

private:
    const std::filesystem::path directory_;
    std::atomic<std::shared_ptr<const response_cache_snapshot>> current_;
};
struct cache_ticket final {
    std::string key_{};
    std::string website_id_{};
    std::string policy_id_{};
    std::string url_{};
    std::uint64_t claim_{};
    std::uint64_t website_epoch_{};
    std::uint64_t policy_epoch_{};
};
struct cache_lookup final {
    std::shared_ptr<const cached_response> response_{};
    std::optional<cache_ticket> ticket_{};
    bool filling_{};
    std::string_view tier_{};
};

// One facade, regex set and in-flight state per data worker.
class response_cache final {
public:
    explicit response_cache(std::shared_ptr<response_cache_publication> publication)
        : publication_(std::move(publication)) {}
    static std::uint64_t epoch(const response_cache_snapshot& snapshot, const std::string& key) {
        const auto found = snapshot.epochs_.find(key);
        return found == snapshot.epochs_.end() ? 0 : found->second;
    }
    bool eligible(std::string_view method, const cache_headers& headers) const {
        const auto control = cache_header(headers, "Cache-Control");
        return (method == "GET" || method == "HEAD") &&
               cache_header(headers, "Authorization").empty() && cache_header(headers, "Cookie").empty() &&
               cache_header(headers, "Range").empty() && cache_header(headers, "Upgrade").empty() &&
               cache_header(headers, "Accept").find("text/event-stream") == std::string::npos &&
               !cache_has_directive(control, "no-cache") && !cache_has_directive(control, "no-store") &&
               !cache_has_directive(cache_header(headers, "Pragma"), "no-cache") &&
               cache_directive_seconds(control, "max-age").value_or(1) != 0;
    }
    const flexedge::cache::rule* matching(const v2::Website& website, std::string_view target, std::string_view method) {
        if (!website.has_response_cache() || !website.response_cache().enabled()) {
            return nullptr;
        }
        const auto identity = website.response_cache().id() + ":" + std::to_string(website.response_cache().revision());
        auto found = policies_.find(identity);
        if (found == policies_.end()) {
            if (policies_.size() > 512) {
                policies_.clear();
            }
            compiled_policy value;
            value.policy_ = flexedge::cache::from_proto(website.response_cache());
            value.patterns_.resize(value.policy_.rules_.size());
            for (std::size_t i = 0; i < value.policy_.rules_.size(); ++i) {
                if (value.policy_.rules_[i].match_type_ == "regex") {
                    for (const auto& pattern : value.policy_.rules_[i].patterns_) {
                        value.patterns_[i].push_back(flexedge::route_policy::compile_pattern(pattern));
                    }
                }
            }
            found = policies_.emplace(identity, std::move(value)).first;
        }
        const auto path = target.substr(0, target.find('?'));
        for (std::size_t i = 0; i < found->second.policy_.rules_.size(); ++i) {
            const auto& rule = found->second.policy_.rules_[i];
            if (!rule.enabled_ || std::ranges::find(rule.methods_, method) == rule.methods_.end()) {
                continue;
            }
            bool matches = rule.match_type_ == "all";
            for (std::size_t p = 0; !matches && p < rule.patterns_.size(); ++p) {
                const auto& pattern = rule.patterns_[p];
                if (rule.match_type_ == "exact") {
                    matches = path == pattern;
                } else if (rule.match_type_ == "prefix") {
                    matches = path.starts_with(pattern);
                } else if (rule.match_type_ == "extension") {
                    matches = path.size() >= pattern.size() && flexedge::route_policy::ascii_equal(path.substr(path.size() - pattern.size()), pattern);
                } else if (rule.match_type_ == "wildcard") {
                    matches = cache_wildcard(path, pattern);
                } else if (rule.match_type_ == "regex" && found->second.patterns_[i][p]) {
                    matches = RE2::PartialMatch(absl::string_view(path.data(), path.size()), *found->second.patterns_[i][p]);
                }
            }
            if (matches) {
                return &rule;
            }
        }
        return nullptr;
    }
    cache_lookup lookup(const v2::Website& website, const flexedge::cache::rule& rule,
        std::string url, const cache_headers& headers, bool acquire = true, buffered_bytes_lease reservation = {}) {
        trim_for_pressure(website.response_cache());
        if (rule.ignore_query_) {
            url.resize(url.find('?') == std::string::npos ? url.size() : url.find('?'));
        }
        std::string key;
        for (const auto& part : {website.id(), std::to_string(website.revision()), website.response_cache().id(), std::to_string(website.response_cache().revision()), rule.id_, url, cache_header(headers, "Accept-Encoding")}) {
            key.append(std::to_string(part.size())).append(":").append(part);
        }
        auto root = publication_->snapshot();
        for (;;) {
            const auto now = std::chrono::steady_clock::now();
            if (const auto entries = root->entries_.find(key); entries != root->entries_.end()) {
                for (const auto& item : entries->second) {
                    if (item->expires_at_ <= now) {
                        continue;
                    }
                    const auto age = item->initial_age_ + std::chrono::duration_cast<std::chrono::seconds>(now - item->stored_at_).count();
                    const auto max_age = cache_directive_seconds(cache_header(headers, "Cache-Control"), "max-age");
                    if (max_age && age > *max_age) {
                        continue;
                    }
                    if (std::ranges::all_of(item->vary_, [&](const auto& vary) { return cache_header(headers, vary.first) == vary.second; })) {
                        if (acquire && website.response_cache().storage() == "tiered" && !item->body_ && item->file_) {
                            return {.response_ = promote(key, item, website.response_cache(), std::move(reservation)), .tier_ = "hit_disk"};
                        }
                        return {.response_ = record_hit(key, item), .tier_ = item->body_ ? "hit_memory" : "hit_disk"};
                    }
                }
            }
            if (!acquire) {
                return {};
            }
            const auto pending = root->filling_.find(key);
            if (pending != root->filling_.end() && pending->second.expires_at_ > now) {
                return {.filling_ = true};
            }
            auto next = std::make_shared<response_cache_snapshot>(*root);
            if (next->filling_.size() >= 4096) {
                std::erase_if(next->filling_, [&](const auto& value) { return value.second.expires_at_ <= now; });
            }
            if (next->filling_.size() >= 4096) {
                return {};
            }
            const auto claim = ++next->sequence_;
            next->filling_[key] = {claim, now + std::chrono::minutes(2)};
            cache_ticket ticket{.key_ = key, .website_id_ = website.id(), .policy_id_ = website.response_cache().id(), .url_ = url, .claim_ = claim, .website_epoch_ = epoch(*root, "website:" + website.id()), .policy_epoch_ = epoch(*root, "policy:" + website.response_cache().id())};
            if (publication_->publish(root, std::shared_ptr<const response_cache_snapshot>(std::move(next)))) {
                return {.ticket_ = std::move(ticket)};
            }
        }
    }
    void abandon(const cache_ticket& ticket) {
        auto root = publication_->snapshot();
        for (;;) {
            const auto found = root->filling_.find(ticket.key_);
            if (found == root->filling_.end() || found->second.id_ != ticket.claim_) {
                return;
            }
            auto next = std::make_shared<response_cache_snapshot>(*root);
            next->filling_.erase(ticket.key_);
            if (publication_->publish(root, std::shared_ptr<const response_cache_snapshot>(std::move(next)))) {
                return;
            }
        }
    }
    bool store(const cache_ticket& ticket, const v2::CachePolicy& policy, const flexedge::cache::rule& rule,
        buffered_proxy_response& response, const cache_headers& request) {
        trim_for_pressure(policy, true);
        const auto control = cache_header(response.headers_, "Cache-Control");
        if (cache_has_directive(control, "private") || cache_has_directive(control, "no-store") || cache_has_directive(control, "no-cache") ||
            (rule.skip_set_cookie_ && !cache_header(response.headers_, "Set-Cookie").empty()) ||
            std::ranges::find(rule.status_codes_, response.status_) == rule.status_codes_.end() ||
            response.body_.size() < static_cast<std::size_t>(rule.min_object_bytes_) ||
            response.body_.size() > static_cast<std::size_t>(rule.max_object_bytes_ ? rule.max_object_bytes_ : policy.max_object_bytes())) {
            abandon(ticket);
            return false;
        }
        if (cache_header(response.headers_, "Content-Type").find("text/event-stream") != std::string::npos) {
            abandon(ticket);
            return false;
        }
        auto seconds = rule.ttl_seconds_;
        const auto wall = std::time(nullptr);
        const auto date = cache_http_time(cache_header(response.headers_, "Date")).value_or(wall);
        const auto apparent_age = (std::max)(std::int64_t{0}, static_cast<std::int64_t>(wall - date));
        std::int64_t upstream_age{};
        const auto age_text = cache_header(response.headers_, "Age");
        if (!age_text.empty()) {
            const auto parsed = std::from_chars(age_text.data(), age_text.data() + age_text.size(), upstream_age);
            if (parsed.ec != std::errc{} || parsed.ptr != age_text.data() + age_text.size() || upstream_age < 0) {
                abandon(ticket);
                return false;
            }
        }
        const auto age = (std::max)(apparent_age, upstream_age);
        if (rule.ttl_mode_ == "origin") {
            auto ttl = cache_directive_seconds(control, "s-maxage");
            if (!ttl) {
                ttl = cache_directive_seconds(control, "max-age");
            }
            if (!ttl) {
                if (const auto expires = cache_http_time(cache_header(response.headers_, "Expires"))) {
                    ttl = static_cast<std::int64_t>(*expires - date);
                }
            }
            seconds = ttl.value_or(0);
        }
        if (seconds <= age) {
            abandon(ticket);
            return false;
        }
        auto item = std::make_shared<cached_response>();
        item->website_id_ = ticket.website_id_;
        item->policy_id_ = ticket.policy_id_;
        item->url_ = ticket.url_;
        item->headers_ = response.headers_;
        item->status_ = response.status_;
        item->body_bytes_ = response.body_.size();
        item->bytes_ = response.body_.size() + 256;
        for (const auto& [name, value] : item->headers_) {
            item->bytes_ += name.size() + value.size();
        }
        item->initial_age_ = age;
        item->browser_ttl_seconds_ = rule.browser_ttl_seconds_;
        item->ignore_query_ = rule.ignore_query_;
        item->stored_at_ = std::chrono::steady_clock::now();
        item->expires_at_ = item->stored_at_ + std::chrono::seconds(seconds - age);
        const auto vary = cache_tokens(cache_header(response.headers_, "Vary"));
        if (vary.size() > 16 || std::ranges::find(vary, "*") != vary.end()) {
            abandon(ticket);
            return false;
        }
        for (const auto& field : vary) {
            if (!flexedge::route_policy::http_token(field)) {
                abandon(ticket);
                return false;
            }
            item->vary_.emplace_back(field, cache_header(request, field));
        }
        if (policy.storage() != "memory") {
            item->file_ = write_file(policy, response.body_);
            if (!item->file_) {
                abandon(ticket);
                return false;
            }
        }
        const auto limits = capacity_limits(policy, *publication_->snapshot());
        if (policy.storage() == "memory" && item->bytes_ > limits.memory_) {
            abandon(ticket);
            return false;
        }
        if (policy.storage() == "memory" || (policy.storage() == "tiered" && item->bytes_ <= limits.memory_)) {
            item->body_ = std::make_shared<const std::string>(response.body_);
        }
        auto root = publication_->snapshot();
        for (;;) {
            const auto claim = root->filling_.find(ticket.key_);
            if (epoch(*root, "website:" + ticket.website_id_) != ticket.website_epoch_ || epoch(*root, "policy:" + ticket.policy_id_) != ticket.policy_epoch_ ||
                claim == root->filling_.end() || claim->second.id_ != ticket.claim_) {
                abandon(ticket);
                return false;
            }
            auto next = std::make_shared<response_cache_snapshot>(*root);
            next->filling_.erase(ticket.key_);
            auto& variants = next->entries_[ticket.key_];
            std::erase_if(variants, [&](const auto& existing) { return existing->vary_ == item->vary_; });
            if (variants.size() >= 16) {
                variants.erase(variants.begin());
            }
            item->sequence_ = ++next->sequence_;
            variants.push_back(std::shared_ptr<const cached_response>(item));
            evict(*next, policy, item->stored_at_, capacity_limits(policy, *next));
            if (!next->entries_.contains(ticket.key_)) {
                abandon(ticket);
                return false;
            }
            // Freeze only after the complete object has been constructed; no writable alias survives publication.
            const auto result = publication_->publish(root, std::shared_ptr<const response_cache_snapshot>(std::move(next)));
            if (result) {
                return true;
            }
        }
    }
    std::shared_ptr<const v2::CacheResult> command_result(const std::string& id) const {
        const auto root = publication_->snapshot();
        const auto found = root->commands_.find(id);
        return found == root->commands_.end() ? nullptr : found->second;
    }
    bool claim_command(const std::string& id) {
        auto root = publication_->snapshot();
        for (;;) {
            if (root->commands_.contains(id)) {
                return false;
            }
            auto next = std::make_shared<response_cache_snapshot>(*root);
            // A null immutable result denotes the currently owned command, without a worker handle.
            next->commands_[id] = {};
            if (next->commands_.size() > 1024) {
                std::erase_if(next->commands_, [](const auto& pair) { return pair.second != nullptr; });
            }
            if (publication_->publish(root, std::shared_ptr<const response_cache_snapshot>(std::move(next)))) {
                return true;
            }
        }
    }
    void complete_command(const v2::CacheResult& result) {
        auto root = publication_->snapshot();
        for (;;) {
            auto next = std::make_shared<response_cache_snapshot>(*root);
            next->commands_[result.attempt_id()] = std::make_shared<const v2::CacheResult>(result);
            if (publication_->publish(root, std::shared_ptr<const response_cache_snapshot>(std::move(next)))) {
                return;
            }
        }
    }
    std::pair<std::uint64_t, std::uint64_t> invalidate(const v2::CacheCommand& command) {
        auto root = publication_->snapshot();
        for (;;) {
            auto next = std::make_shared<response_cache_snapshot>(*root);
            if (command.operation() == "clean") {
                ++next->epochs_["policy:" + command.policy_id()];
            } else {
                for (const auto& site : command.website_ids()) {
                    ++next->epochs_["website:" + site];
                }
            }
            std::uint64_t count{}, bytes{};
            const auto now = std::chrono::steady_clock::now();
            for (auto at = next->entries_.begin(); at != next->entries_.end();) {
                std::erase_if(at->second, [&](const auto& item) {
                    bool matched = item->policy_id_ == command.policy_id() &&
                                   (command.operation() == "clean" || std::ranges::find(command.website_ids(), item->website_id_) != command.website_ids().end());
                    if (matched && command.scope() == "expired") {
                        matched = item->expires_at_ <= now;
                    }
                    if (matched && (command.scope() == "urls" || command.scope() == "prefix")) {
                        matched = false;
                        for (const auto& url : command.urls()) {
                            auto candidate = url;
                            if (item->ignore_query_) {
                                candidate.resize(candidate.find('?') == std::string::npos ? candidate.size() : candidate.find('?'));
                            }
                            if (command.scope() == "prefix" ? item->url_.starts_with(candidate) : item->url_ == candidate) {
                                matched = true;
                                break;
                            }
                        }
                    }
                    if (matched) {
                        ++count;
                        bytes += item->bytes_;
                    }
                    return matched;
                });
                if (at->second.empty()) {
                    at = next->entries_.erase(at);
                } else {
                    ++at;
                }
            }
            if (publication_->publish(root, std::shared_ptr<const response_cache_snapshot>(std::move(next)))) {
                return {count, bytes};
            }
        }
    }

private:
    std::shared_ptr<const cached_response> record_hit(const std::string& key,
        std::shared_ptr<const cached_response> object) {
        auto root = publication_->snapshot();
        for (;;) {
            const auto found = root->entries_.find(key);
            if (found == root->entries_.end() || std::ranges::find(found->second, object) == found->second.end()) {
                return object;
            }
            auto replacement = std::make_shared<response_cache_snapshot>(*root);
            auto touched = std::make_shared<cached_response>(*object);
            touched->sequence_ = ++replacement->sequence_;
            auto& variants = replacement->entries_.at(key);
            *std::ranges::find(variants, object) = std::shared_ptr<const cached_response>(touched);
            if (publication_->publish(root, std::shared_ptr<const response_cache_snapshot>(std::move(replacement)))) {
                return touched;
            }
        }
    }
    struct compiled_policy final {
        flexedge::cache::policy policy_{};
        std::vector<std::vector<std::shared_ptr<const RE2>>> patterns_{};
    };
    struct capacity final {
        std::uint64_t memory_{}, disk_{};
    };
    static capacity usage(const response_cache_snapshot& root, const std::string& policy_id) {
        capacity result;
        for (const auto& [key, variants] : root.entries_) {
            (void)key;
            for (const auto& item : variants) {
                if (item->policy_id_ != policy_id) {
                    continue;
                }
                result.memory_ += item->bytes_ - (item->body_ ? 0 : item->body_bytes_);
                if (item->file_) {
                    result.disk_ += item->bytes_;
                }
            }
        }
        return result;
    }
    capacity capacity_limits(const v2::CachePolicy& policy, const response_cache_snapshot& root) const {
        const auto used = usage(root, policy.id());
        capacity result{static_cast<std::uint64_t>(policy.memory_bytes()), static_cast<std::uint64_t>(policy.disk_bytes())};
        if (policy.memory_percent() || policy.memory_reserved_bytes()) {
            result.memory_ = cache_capacity_limit(cache_memory_capacity(), used.memory_, result.memory_,
                policy.memory_percent(), policy.memory_reserved_bytes());
        }
        if (policy.disk_percent() || policy.disk_reserved_bytes()) {
            std::error_code error;
            const auto space = std::filesystem::space(publication_->directory(), error);
            result.disk_ = cache_capacity_limit(error ? std::nullopt : std::optional<cache_resource_capacity>{{space.capacity, space.available}},
                used.disk_, result.disk_, policy.disk_percent(), policy.disk_reserved_bytes());
        }
        return result;
    }
    void trim_for_pressure(const v2::CachePolicy& policy, bool force = false) {
        if (!policy.memory_percent() && !policy.disk_percent() && !policy.memory_reserved_bytes() && !policy.disk_reserved_bytes()) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (pressure_checks_.size() > 512) {
            pressure_checks_.clear();
        }
        auto& checked = pressure_checks_[policy.id()];
        if (!force && now - checked < std::chrono::seconds(1)) {
            return;
        }
        checked = now;
        auto root = publication_->snapshot();
        for (;;) {
            const auto used = usage(*root, policy.id());
            const auto limits = capacity_limits(policy, *root);
            if (used.memory_ <= limits.memory_ && used.disk_ <= limits.disk_) {
                return;
            }
            auto next = std::make_shared<response_cache_snapshot>(*root);
            evict(*next, policy, now, limits);
            if (publication_->publish(root, std::shared_ptr<const response_cache_snapshot>(std::move(next)))) {
                return;
            }
        }
    }
    std::shared_ptr<const cache_file> write_file(const v2::CachePolicy& policy, std::string_view bytes) {
        std::filesystem::path temporary;
        try {
            const auto space = std::filesystem::space(publication_->directory());
            if (space.available < static_cast<std::uint64_t>(policy.disk_reserved_bytes()) ||
                bytes.size() > space.available - static_cast<std::uint64_t>(policy.disk_reserved_bytes())) {
                return {};
            }
            auto directory = publication_->directory() / policy.directory() / policy.id();
            auto current = publication_->directory();
            std::filesystem::create_directories(current);
            for (const auto& part : std::filesystem::path(policy.directory()) / policy.id()) {
                current /= part;
                if (std::filesystem::is_symlink(current)) {
                    return {};
                }
                std::filesystem::create_directories(current);
            }
            std::random_device random;
            std::string suffix;
            constexpr char alphabet[] = "0123456789abcdef";
            for (int i = 0; i < 32; ++i) {
                suffix.push_back(alphabet[random() & 15]);
            }
            temporary = directory / (suffix + ".tmp");
            const auto final = directory / (suffix + ".body");
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            output.close();
            if (!output) {
                std::filesystem::remove(temporary);
                return {};
            }
            std::filesystem::rename(temporary, final);
            return std::make_shared<const cache_file>(final);
        } catch (const std::filesystem::filesystem_error&) {
            std::error_code ignored;
            if (!temporary.empty()) {
                std::filesystem::remove(temporary, ignored);
            }
            return {};
        }
    }
    std::shared_ptr<const cached_response> promote(const std::string& key,
        std::shared_ptr<const cached_response> object, const v2::CachePolicy& policy,
        buffered_bytes_lease reservation) {
        if (object->bytes_ > capacity_limits(policy, *publication_->snapshot()).memory_ ||
            !reservation.try_grow(object->body_bytes_)) {
            return object;
        }
        std::ifstream input(object->file_->path_, std::ios::binary);
        if (!input) {
            return object;
        }
        std::string bytes(object->body_bytes_, '\0');
        input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (static_cast<std::size_t>(input.gcount()) != bytes.size()) {
            return object;
        }
        auto promoted = std::make_shared<cached_response>(*object);
        promoted->body_ = std::make_shared<const std::string>(std::move(bytes));
        auto root = publication_->snapshot();
        for (;;) {
            const auto found = root->entries_.find(key);
            if (found == root->entries_.end() || std::ranges::find(found->second, object) == found->second.end() ||
                object->expires_at_ <= std::chrono::steady_clock::now()) {
                return object;
            }
            auto replacement = std::make_shared<response_cache_snapshot>(*root);
            promoted->sequence_ = ++replacement->sequence_;
            auto& variants = replacement->entries_.at(key);
            *std::ranges::find(variants, object) = std::shared_ptr<const cached_response>(promoted);
            evict(*replacement, policy, std::chrono::steady_clock::now(), capacity_limits(policy, *replacement));
            auto result = std::shared_ptr<const cached_response>(promoted);
            const auto retained = replacement->entries_.find(key);
            if (retained != replacement->entries_.end()) {
                for (const auto& item : retained->second) {
                    if (item->file_ == object->file_) {
                        result = item;
                        break;
                    }
                }
            }
            if (publication_->publish(root, std::shared_ptr<const response_cache_snapshot>(std::move(replacement)))) {
                return result;
            }
        }
    }
    static void evict(response_cache_snapshot& snapshot, const v2::CachePolicy& policy,
        std::chrono::steady_clock::time_point now, capacity limits) {
        std::vector<std::pair<std::string, std::shared_ptr<const cached_response>>> items;
        std::uint64_t memory{}, disk{};
        const auto memory_cost = [](const auto& item) { return item->bytes_ - (item->body_ ? 0 : item->body_bytes_); };
        for (auto at = snapshot.entries_.begin(); at != snapshot.entries_.end();) {
            std::erase_if(at->second, [&](const auto& item) { return item->policy_id_ == policy.id() && item->expires_at_ <= now; });
            for (const auto& item : at->second) {
                if (item->policy_id_ == policy.id()) {
                    memory += memory_cost(item);
                    if (item->file_) {
                        disk += item->bytes_;
                    }
                    items.emplace_back(at->first, item);
                }
            }
            if (at->second.empty()) {
                at = snapshot.entries_.erase(at);
            } else {
                ++at;
            }
        }
        std::ranges::sort(items, [](const auto& left, const auto& right) { return left.second->sequence_ < right.second->sequence_; });
        const auto remove = [&](const std::string& key, const auto& item) {
            const auto found = snapshot.entries_.find(key);
            if (found == snapshot.entries_.end() || std::ranges::find(found->second, item) == found->second.end()) {
                return;
            }
            memory -= memory_cost(item);
            if (item->file_) {
                disk -= item->bytes_;
            }
            std::erase(found->second, item);
            if (found->second.empty()) {
                snapshot.entries_.erase(found);
            }
        };
        for (auto& [key, item] : items) {
            if (disk <= limits.disk_ && snapshot.entries_.size() <= 16384) {
                break;
            }
            remove(key, item);
        }
        for (auto& [key, item] : items) {
            if (memory <= limits.memory_) {
                break;
            }
            const auto found = snapshot.entries_.find(key);
            if (found == snapshot.entries_.end() || !item->body_ || std::ranges::find(found->second, item) == found->second.end()) {
                continue;
            }
            if (item->file_) {
                auto disk_only = std::make_shared<cached_response>(*item);
                disk_only->body_.reset();
                *std::ranges::find(found->second, item) = std::shared_ptr<const cached_response>(disk_only);
                memory -= item->body_bytes_;
                item = std::move(disk_only);
            } else {
                remove(key, item);
            }
        }
        for (const auto& [key, item] : items) {
            if (memory <= limits.memory_) {
                break;
            }
            remove(key, item);
        }
    }
    std::shared_ptr<response_cache_publication> publication_;
    std::unordered_map<std::string, compiled_policy> policies_{};
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> pressure_checks_{};
};
}  // namespace flexedge::node
