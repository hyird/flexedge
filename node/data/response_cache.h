#pragma once
#include <array>
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
#include <span>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common/cache_policy.h"
#include "common/cache_url.h"
#include "node/data/buffered_bytes_budget.h"
#include "node/data/cache_capacity.h"
#include "node/data/cache_disk_record.h"
#include "node/data/cache_response_head.h"
#include "node/data/response_compression.h"
#include "node/proto/edge_control.pb.h"

namespace flexedge::node {
using cache_headers = std::vector<std::pair<std::string, std::string>>;

struct cache_invalidation_url final {
    std::string website_id_{};
    flexedge::cache::url url_{};
};

// The website owns the cached representation. Its domain aliases do not create
// separate objects; protocol, target and Vary still distinguish representations.
inline std::pair<std::string_view, std::string_view> cache_resource_address(std::string_view url) {
    const auto separator = url.find("://");
    const auto target = separator == std::string_view::npos ? separator : url.find('/', separator + 3);
    if (target == std::string_view::npos) {
        throw std::invalid_argument("cache URL must be normalized");
    }
    return {url.substr(0, separator), url.substr(target)};
}

inline std::string cache_file_name() {
    std::random_device random;
    constexpr char digits[] = "0123456789abcdef";
    std::string value;
    value.reserve(32);
    // Consume all 32 random bits per draw. Drawing once per hex digit wastes
    // 7/8 of the entropy and is expensive on Linux/hypervisor random devices.
    for (unsigned word = 0; word < 4; ++word) {
        auto bits = static_cast<std::uint32_t>(random());
        for (unsigned digit = 0; digit < 8; ++digit) {
            value.push_back(digits[bits & 15]);
            bits >>= 4;
        }
    }
    return value;
}

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

struct cache_byte_range final {
    std::size_t offset_{}, length_{};
    bool satisfiable_{};
};
inline std::optional<cache_byte_range> parse_cache_range(std::string_view field, std::size_t size) {
    if (!field.starts_with("bytes=") || field.find(',') != std::string_view::npos) {
        return std::nullopt;
    }
    field.remove_prefix(6);
    const auto dash = field.find('-');
    if (dash == std::string_view::npos) {
        return std::nullopt;
    }
    const auto number = [](std::string_view value) -> std::optional<std::size_t> {
        std::size_t result{};
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
        if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
            return std::nullopt;
        }
        return result;
    };
    if (dash == 0) {
        const auto suffix = number(field.substr(1));
        if (!suffix) {
            return std::nullopt;
        }
        const auto length = (std::min)(*suffix, size);
        return cache_byte_range{size - length, length, length != 0};
    }
    const auto start = number(field.substr(0, dash));
    if (!start) {
        return std::nullopt;
    }
    const auto tail = field.substr(dash + 1);
    const auto end = tail.empty() ? std::optional<std::size_t>{size ? size - 1 : 0} : number(tail);
    if (!end) {
        return std::nullopt;
    }
    if (*start >= size || *end < *start) {
        return cache_byte_range{};
    }
    return cache_byte_range{*start, (std::min)(*end, size - 1) - *start + 1, true};
}

// Published objects and every value reachable from them are immutable.
// File handles and response cursors belong to the reading worker.
struct cache_file final {
    explicit cache_file(std::filesystem::path path)
        : path_(std::move(path)) {}
    ~cache_file() {
        try {
            std::error_code ignored;
            auto metadata = path_;
            metadata.replace_extension(".meta");
            if (!std::filesystem::exists(metadata, ignored)) {
                std::filesystem::remove(path_, ignored);
            }
        } catch (...) {
        }
    }
    const std::filesystem::path path_;
};

// One worker owns each fill. Disk writes are bounded by the current response chunk;
// only memory/tiered fills retain an optional, budgeted in-memory copy.
class cache_fill final {
public:
    cache_fill(cache_response_head head, std::size_t maximum, std::size_t memory_limit,
        bool memory_required, buffered_bytes_lease reservation)
        : head_(std::move(head)),
          reservation_(std::move(reservation)),
          maximum_(maximum),
          memory_limit_(memory_limit),
          memory_required_(memory_required) {}
    ~cache_fill() {
        discard();
    }
    cache_fill(const cache_fill&) = delete;
    cache_fill& operator=(const cache_fill&) = delete;

    bool append(std::string_view bytes) {
        if (!active_ || bytes.size() > maximum_ - body_bytes_) {
            failure_reason_ = active_ ? "object_too_large" : "cache_write_failed";
            discard();
            return false;
        }
        if (output_.is_open()) {
            output_.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            output_.flush();
            if (!output_) {
                failure_reason_ = "cache_write_failed";
                discard();
                return false;
            }
        }
        if (retain_memory_) {
            if (bytes.size() > memory_limit_ - (std::min)(memory_limit_, memory_.size()) ||
                !reservation_.try_grow(bytes.size())) {
                drop_memory();
            } else {
                try {
                    memory_.append(bytes);
                } catch (const std::bad_alloc&) {
                    drop_memory();
                }
            }
        }
        if (memory_required_ && !retain_memory_) {
            failure_reason_ = "memory_limit";
            discard();
            return false;
        }
        body_bytes_ += bytes.size();
        return true;
    }
    [[nodiscard]] bool active() const noexcept {
        return active_;
    }
    [[nodiscard]] std::string_view failure_reason() const noexcept {
        return failure_reason_;
    }
    [[nodiscard]] std::size_t body_bytes() const noexcept {
        return body_bytes_;
    }
    [[nodiscard]] std::size_t memory_bytes() const noexcept {
        return memory_.size();
    }

private:
    friend class response_cache;
    std::string_view failure_reason_{};
    void drop_memory() noexcept {
        retain_memory_ = false;
        std::string().swap(memory_);
        reservation_ = {};
    }
    void discard() noexcept {
        active_ = false;
        if (output_.is_open()) {
            output_.close();
        }
        if (!temporary_.empty()) {
            std::error_code ignored;
            std::filesystem::remove(temporary_, ignored);
            temporary_.clear();
        }
        drop_memory();
    }
    bool open_file(const std::filesystem::path& root, const v2::CachePolicy& policy) {
        try {
            const auto space = std::filesystem::space(root);
            const auto reserved = static_cast<std::uint64_t>(policy.disk_reserved_bytes());
            if (space.available <= reserved) {
                return false;
            }
            maximum_ = static_cast<std::size_t>((std::min)(static_cast<std::uint64_t>(maximum_), space.available - reserved));
            auto directory = root;
            for (const auto& part : std::filesystem::path(policy.directory()) / policy.id()) {
                directory /= part;
                if (std::filesystem::is_symlink(directory)) {
                    return false;
                }
                std::filesystem::create_directories(directory);
            }
            temporary_ = directory / (cache_file_name() + ".tmp");
            output_.open(temporary_, std::ios::binary | std::ios::trunc);
            return static_cast<bool>(output_);
        } catch (const std::filesystem::filesystem_error&) {
            return false;
        }
    }
    std::shared_ptr<const cache_file> finish_file() {
        if (temporary_.empty()) {
            return {};
        }
        output_.close();
        if (!output_) {
            return {};
        }
        auto final = temporary_;
        final.replace_extension(".body");
        // Allocate ownership before rename so allocation failure still removes the temp.
        auto file = std::make_shared<const cache_file>(final);
        std::error_code error;
        std::filesystem::rename(temporary_, final, error);
        if (error) {
            return {};
        }
        temporary_.clear();
        return file;
    }
    cache_response_head head_;
    buffered_bytes_lease reservation_;
    std::string memory_{};
    std::filesystem::path temporary_{};
    std::ofstream output_{};
    std::size_t maximum_{};
    std::size_t memory_limit_{};
    std::size_t body_bytes_{};
    bool memory_required_{};
    bool retain_memory_{true};
    bool active_{true};
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
    std::uint64_t identity_{};
    std::uint64_t website_epoch_{}, policy_epoch_{};
    std::int64_t stale_if_error_seconds_{};
    std::int64_t initial_age_{};
    std::int64_t browser_ttl_seconds_{};
    bool ignore_query_{};
    std::chrono::steady_clock::time_point stored_at_{};
    std::chrono::steady_clock::time_point expires_at_{};
    std::chrono::steady_clock::time_point retain_until_{};
};
struct cache_fill_claim final {
    std::uint64_t id_{};
    std::chrono::steady_clock::time_point expires_at_{};
    std::string website_id_{}, policy_id_{};
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
        std::filesystem::create_directories(directory_);
        for (auto parent = directory_; !parent.empty() && parent != parent.root_path(); parent = parent.parent_path()) {
            if (std::filesystem::is_symlink(parent)) {
                throw std::runtime_error("cache directory cannot contain symlinks");
            }
        }
        auto restored = std::make_shared<response_cache_snapshot>();
        std::vector<std::filesystem::path> records;
        std::unordered_map<std::string, std::pair<std::uint64_t, std::filesystem::path>> epoch_records;
        std::vector<std::filesystem::path> old_epochs;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(directory_)) {
            if (!entry.is_regular_file() || entry.is_symlink()) {
                continue;
            }
            const auto name = entry.path().stem().string();
            if (name.size() != 32 || !std::ranges::all_of(name, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); })) {
                continue;
            }
            if (entry.path().extension() == ".epoch" && entry.file_size() < 4096) {
                std::ifstream input(entry.path());
                std::string key;
                std::uint64_t value{};
                if (input >> std::quoted(key) >> value) {
                    restored->epochs_[key] = (std::max)(restored->epochs_[key], value);
                    auto& latest = epoch_records[key];
                    if (latest.second.empty() || value > latest.first) {
                        if (!latest.second.empty()) {
                            old_epochs.push_back(latest.second);
                        }
                        latest = {value, entry.path()};
                    } else {
                        old_epochs.push_back(entry.path());
                    }
                }
            } else if (entry.path().extension() == ".meta") {
                records.push_back(entry.path());
            }
        }
        for (const auto& path : old_epochs) {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
        for (const auto& path : records) {
            restore(path, *restored);
        }
        for (const auto& entry : std::filesystem::recursive_directory_iterator(directory_)) {
            if (!entry.is_regular_file() || entry.is_symlink()) {
                continue;
            }
            const auto name = entry.path().stem().string(), extension = entry.path().extension().string();
            if (name.size() != 32 || !std::ranges::all_of(name, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); })) {
                continue;
            }
            auto metadata = entry.path();
            metadata.replace_extension(".meta");
            if (extension == ".tmp" || (extension == ".body" && !std::filesystem::exists(metadata))) {
                std::error_code ignored;
                std::filesystem::remove(entry.path(), ignored);
            }
        }
        current_.store(std::shared_ptr<const response_cache_snapshot>(std::move(restored)));
    }

    std::shared_ptr<const response_cache_snapshot> snapshot() const {
        return current_.load(std::memory_order_acquire);
    }
    bool publish(std::shared_ptr<const response_cache_snapshot>& expected,
        std::shared_ptr<const response_cache_snapshot> replacement, bool retire = false) {
        std::vector<std::shared_ptr<const cache_file>> removed;
        if (retire) {
            std::unordered_set<std::string> live;
            for (const auto& [key, entries] : replacement->entries_) {
                (void)key;
                for (const auto& item : entries) {
                    if (item->file_) {
                        live.insert(item->file_->path_.string());
                    }
                }
            }
            for (const auto& [key, entries] : expected->entries_) {
                (void)key;
                for (const auto& item : entries) {
                    if (item->file_ && !live.contains(item->file_->path_.string())) {
                        removed.push_back(item->file_);
                    }
                }
            }
        }
        if (!current_.compare_exchange_weak(expected, std::move(replacement), std::memory_order_release, std::memory_order_acquire)) {
            return false;
        }
        for (const auto& file : removed) {
            erase_metadata(*file);
        }
        return true;
    }
    static void erase_metadata(const cache_file& file) noexcept {
        try {
            auto path = file.path_;
            path.replace_extension(".meta");
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        } catch (...) {
        }
    }
    void persist_epoch(const std::string& key, std::uint64_t value) const {
        const auto path = directory_ / (cache_file_name() + ".epoch");
        auto temporary = path;
        temporary.replace_extension(".tmp");
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << std::quoted(key) << ' ' << value;
        output.close();
        if (!output) {
            throw std::runtime_error("cannot persist cache invalidation");
        }
        std::filesystem::rename(temporary, path);
    }
    bool persist(const std::string& key, const cached_response& item) const {
        if (!item.file_) {
            return true;
        }
        cache_disk_record record;
        record.key_ = key;
        record.website_ = item.website_id_;
        record.policy_ = item.policy_id_;
        record.url_ = item.url_;
        record.headers_ = item.headers_;
        record.vary_ = item.vary_;
        const auto wall = std::time(nullptr);
        const auto now = std::chrono::steady_clock::now();
        const auto seconds = [&](auto point) { return std::chrono::duration_cast<std::chrono::seconds>(point - now).count(); };
        const auto age = item.initial_age_ + std::chrono::duration_cast<std::chrono::seconds>(now - item.stored_at_).count();
        record.values_ = {item.identity_, item.sequence_, item.body_bytes_, item.status_, static_cast<std::uint64_t>((std::max)(std::int64_t{0}, age)),
            static_cast<std::uint64_t>(wall), static_cast<std::uint64_t>(wall + seconds(item.expires_at_)),
            static_cast<std::uint64_t>(wall + seconds(item.retain_until_)), static_cast<std::uint64_t>(item.browser_ttl_seconds_),
            static_cast<std::uint64_t>(item.stale_if_error_seconds_), item.ignore_query_ ? 1u : 0u, item.website_epoch_, item.policy_epoch_};
        auto path = item.file_->path_, temporary = path;
        path.replace_extension(".meta");
        temporary = path.parent_path() / (cache_file_name() + ".tmp");
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        const bool written = record.write(output);
        output.close();
        std::error_code error;
        if (written && output) {
#ifdef _WIN32
            // The complete body remains pinned; epochs make a missing metadata file
            // during replacement a cold miss after a crash, never a stale resurrection.
            std::filesystem::remove(path, error);
            error.clear();
#endif
            std::filesystem::rename(temporary, path, error);
            if (!error) {
                return true;
            }
        }
        std::filesystem::remove(temporary, error);
        return false;
    }
    const std::filesystem::path& directory() const {
        return directory_;
    }

private:
    static void restore(const std::filesystem::path& path, response_cache_snapshot& root) {
        auto body = path;
        body.replace_extension(".body");
        const auto discard = [&] { std::error_code ignored; std::filesystem::remove(path, ignored); std::filesystem::remove(body, ignored); };
        try {
            if (std::filesystem::file_size(path) > 1024 * 1024 || std::filesystem::is_symlink(body)) {
                discard();
                return;
            }
            std::ifstream input(path, std::ios::binary);
            cache_disk_record record;
            if (!record.read(input) || record.website_.empty() || record.policy_.empty() || record.key_.size() > 16384 ||
                record.values_[2] > flexedge::cache::maximum_object_bytes || record.values_[3] < 200 || record.values_[3] > 599 ||
                record.values_[7] <= static_cast<std::uint64_t>(std::time(nullptr)) || record.vary_.size() > 16 ||
                record.values_[4] > 31536000 || record.values_[5] > 4102444800ULL || record.values_[6] > 4102444800ULL || record.values_[7] > 4102444800ULL ||
                record.values_[8] > 31536000 || record.values_[9] > 3600 || record.website_.size() > 128 || record.policy_.size() > 128 || record.url_.size() > 8192 ||
                record.values_[11] != root.epochs_["website:" + record.website_] ||
                record.values_[12] != root.epochs_["policy:" + record.policy_] ||
                std::filesystem::file_size(body) != record.values_[2]) {
                discard();
                return;
            }
            for (const auto& [name, value] : record.headers_) {
                if (!flexedge::route_policy::http_token(name) || value.find_first_of("\r\n\0", 0, 3) != std::string::npos) {
                    discard();
                    return;
                }
            }
            auto item = std::make_shared<cached_response>();
            item->website_id_ = record.website_;
            item->policy_id_ = record.policy_;
            item->url_ = record.url_;
            item->headers_ = std::move(record.headers_);
            item->vary_ = std::move(record.vary_);
            item->identity_ = record.values_[0];
            item->sequence_ = record.values_[1];
            item->body_bytes_ = record.values_[2];
            item->status_ = static_cast<std::uint32_t>(record.values_[3]);
            const auto wall = static_cast<std::int64_t>(std::time(nullptr));
            const auto now = std::chrono::steady_clock::now();
            item->initial_age_ = static_cast<std::int64_t>(record.values_[4]) + (std::max)(std::int64_t{0}, wall - static_cast<std::int64_t>(record.values_[5]));
            item->stored_at_ = now;
            item->expires_at_ = now + std::chrono::seconds(static_cast<std::int64_t>(record.values_[6]) - wall);
            item->retain_until_ = now + std::chrono::seconds(static_cast<std::int64_t>(record.values_[7]) - wall);
            item->browser_ttl_seconds_ = static_cast<std::int64_t>(record.values_[8]);
            item->stale_if_error_seconds_ = static_cast<std::int64_t>(record.values_[9]);
            item->ignore_query_ = record.values_[10] != 0;
            item->website_epoch_ = record.values_[11];
            item->policy_epoch_ = record.values_[12];
            item->file_ = std::make_shared<const cache_file>(body);
            item->bytes_ = item->body_bytes_ + 256;
            for (const auto& field : item->headers_) {
                item->bytes_ += field.first.size() + field.second.size();
            }
            root.sequence_ = (std::max)(root.sequence_, item->sequence_);
            root.entries_[record.key_].push_back(std::shared_ptr<const cached_response>(std::move(item)));
        } catch (const std::exception&) {
            discard();
        }
    }
    const std::filesystem::path directory_;
    std::atomic<std::shared_ptr<const response_cache_snapshot>> current_;
};
struct cache_ticket final {
    std::string key_{};
    std::string website_id_{};
    std::string policy_id_{};
    std::string url_{};
    std::string encoding_{};
    std::uint64_t claim_{};
    std::uint64_t website_epoch_{};
    std::uint64_t policy_epoch_{};
};
struct cache_lookup final {
    std::shared_ptr<const cached_response> response_{};
    std::shared_ptr<const cached_response> stale_{};
    std::optional<cache_ticket> ticket_{};
    bool filling_{};
    std::string_view tier_{};
    std::string key_{};
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
    static std::string_view request_bypass_reason(std::string_view method, const cache_headers& headers) {
        if (method != "GET" && method != "HEAD") {
            return "request_method";
        }
        if (!cache_header(headers, "Authorization").empty()) {
            return "request_authorization";
        }
        if (!cache_header(headers, "Cookie").empty()) {
            return "request_cookie";
        }
        if (!cache_header(headers, "Upgrade").empty()) {
            return "request_upgrade";
        }
        if (accepts_event_stream(cache_header(headers, "Accept"))) {
            return "request_event_stream";
        }
        if (cache_has_directive(cache_header(headers, "Cache-Control"), "no-store")) {
            return "request_no_store";
        }
        return {};
    }
    static bool requires_validation(const cache_headers& headers) {
        const auto control = cache_header(headers, "Cache-Control");
        return cache_has_directive(control, "no-cache") ||
               cache_has_directive(cache_header(headers, "Pragma"), "no-cache") ||
               cache_directive_seconds(control, "max-age").value_or(1) == 0;
    }
    bool eligible(std::string_view method, const cache_headers& headers) const {
        return request_bypass_reason(method, headers).empty();
    }
    static std::string_view response_bypass_reason(const flexedge::cache::rule& rule, const cache_response_head& head) {
        const auto control = cache_header(head.headers_, "Cache-Control");
        if (cache_has_directive(control, "private")) {
            return "origin_private";
        }
        if (cache_has_directive(control, "no-store")) {
            return "origin_no_store";
        }
        if (rule.skip_no_cache_ && cache_has_directive(control, "no-cache")) {
            return "origin_no_cache";
        }
        if (rule.skip_set_cookie_ && !cache_header(head.headers_, "Set-Cookie").empty()) {
            return "origin_set_cookie";
        }
        if (std::ranges::find(rule.status_codes_, head.status_) == rule.status_codes_.end()) {
            return "origin_status";
        }
        if (is_event_stream_content_type(cache_header(head.headers_, "Content-Type"))) {
            return "origin_event_stream";
        }
        const auto vary = cache_tokens(cache_header(head.headers_, "Vary"));
        if (vary.size() > 16 || std::ranges::find(vary, "*") != vary.end() ||
            !std::ranges::all_of(vary, [](const auto& field) { return flexedge::route_policy::http_token(field); })) {
            return "origin_vary";
        }
        return {};
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
        std::string url, const cache_headers& headers, bool acquire = true, bool existing_only = false) {
        trim_for_pressure(website.response_cache());
        if (rule.ignore_query_) {
            url.resize(url.find('?') == std::string::npos ? url.size() : url.find('?'));
        }
        const auto encoding = cache_header(headers, "Range").empty() ? ruvia::http_content_coding_token(preferred_response_coding(website, cache_header(headers, "Accept-Encoding"))) : std::string_view("identity");
        const auto website_revision = std::to_string(website.revision());
        const auto policy_revision = std::to_string(website.response_cache().revision());
        const auto [scheme, target] = cache_resource_address(url);
        const std::array<std::string_view, 8> parts{website.id(), website_revision, website.response_cache().id(), policy_revision, rule.id_, scheme, target, encoding};
        std::string key;
        std::size_t capacity{};
        for (const auto part : parts) {
            capacity += part.size() + 2;  // First length digit and colon.
            for (auto length = part.size(); length >= 10; length /= 10) {
                ++capacity;
            }
        }
        key.reserve(capacity);
        for (const auto part : parts) {
            key.append(std::to_string(part.size())).append(":").append(part);
        }
        auto root = publication_->snapshot();
        for (;;) {
            const auto now = std::chrono::steady_clock::now();
            std::shared_ptr<const cached_response> stale;
            if (const auto entries = root->entries_.find(key); entries != root->entries_.end()) {
                for (const auto& item : entries->second) {
                    if (item->retain_until_ <= now || !std::ranges::all_of(item->vary_, [&](const auto& vary) {
                            return (vary.first == "accept-encoding" ? encoding : cache_header(headers, vary.first)) == vary.second;
                        })) {
                        continue;
                    }
                    const auto age = item->initial_age_ + std::chrono::duration_cast<std::chrono::seconds>(now - item->stored_at_).count();
                    const auto max_age = rule.respect_request_no_cache_ ? cache_directive_seconds(cache_header(headers, "Cache-Control"), "max-age") : std::nullopt;
                    if (item->expires_at_ > now && (!max_age || age <= *max_age)) {
                        return {.response_ = record_hit(key, item), .tier_ = item->body_ ? "hit_memory" : "hit_disk", .key_ = std::move(key)};
                    }
                    if (!stale || item->sequence_ > stale->sequence_) {
                        stale = item;
                    }
                }
            }
            if (!acquire || (existing_only && !stale)) {
                return {.stale_ = std::move(stale), .key_ = std::move(key)};
            }
            const auto pending = root->filling_.find(key);
            if (pending != root->filling_.end() && pending->second.expires_at_ > now) {
                return {.stale_ = std::move(stale), .filling_ = true, .key_ = std::move(key)};
            }
            auto next = std::make_shared<response_cache_snapshot>(*root);
            if (next->filling_.size() >= 4096) {
                std::erase_if(next->filling_, [&](const auto& value) { return value.second.expires_at_ <= now; });
            }
            if (next->filling_.size() >= 4096) {
                return {.stale_ = std::move(stale), .key_ = std::move(key)};
            }
            const auto claim = ++next->sequence_;
            next->filling_[key] = {claim, now + std::chrono::seconds(30), website.id(), website.response_cache().id()};
            cache_ticket ticket{.key_ = key, .website_id_ = website.id(), .policy_id_ = website.response_cache().id(), .url_ = url, .encoding_ = std::string(encoding), .claim_ = claim, .website_epoch_ = epoch(*root, "website:" + website.id()), .policy_epoch_ = epoch(*root, "policy:" + website.response_cache().id())};
            if (publication_->publish(root, std::shared_ptr<const response_cache_snapshot>(std::move(next)))) {
                return {.stale_ = std::move(stale), .ticket_ = std::move(ticket), .key_ = std::move(key)};
            }
        }
    }
    bool renew(const cache_ticket& ticket) {
        auto root = publication_->snapshot();
        for (;;) {
            const auto found = root->filling_.find(ticket.key_);
            if (found == root->filling_.end() || found->second.id_ != ticket.claim_ ||
                epoch(*root, "website:" + ticket.website_id_) != ticket.website_epoch_ ||
                epoch(*root, "policy:" + ticket.policy_id_) != ticket.policy_epoch_) {
                return false;
            }
            auto next = std::make_shared<response_cache_snapshot>(*root);
            next->filling_[ticket.key_].expires_at_ = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            if (publication_->publish(root, std::shared_ptr<const response_cache_snapshot>(std::move(next)))) {
                return true;
            }
        }
    }
    bool current(const std::string& key, const cached_response& object) const {
        const auto root = publication_->snapshot();
        const auto found = root->entries_.find(key);
        return found != root->entries_.end() && std::ranges::any_of(found->second, [&](const auto& item) { return item->identity_ == object.identity_; });
    }
    static bool usable_stale(const cached_response& object) {
        return object.stale_if_error_seconds_ > 0 && std::chrono::steady_clock::now() < object.expires_at_ + std::chrono::seconds(object.stale_if_error_seconds_);
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
    std::unique_ptr<cache_fill> begin_fill(const v2::CachePolicy& policy, const flexedge::cache::rule& rule,
        const cache_response_head& head, buffered_bytes_lease reservation) {
        if (!response_bypass_reason(rule, head).empty()) {
            return {};
        }
        const auto limits = capacity_limits(policy, *publication_->snapshot());
        const auto maximum = static_cast<std::size_t>(rule.max_object_bytes_ ? rule.max_object_bytes_ : policy.max_object_bytes());
        const auto memory_limit = static_cast<std::size_t>((std::min)(limits.memory_, static_cast<std::uint64_t>(maximum)));
        auto fill = std::make_unique<cache_fill>(head, maximum, memory_limit, policy.storage() == "memory", std::move(reservation));
        if (policy.storage() != "memory" && !fill->open_file(publication_->directory(), policy)) {
            return {};
        }
        if (policy.storage() == "disk") {
            fill->drop_memory();
        }
        return fill;
    }
    bool store(const cache_ticket& ticket, const v2::CachePolicy& policy, const flexedge::cache::rule& rule,
        cache_fill& fill, const cache_headers& request) {
        flush_touches();
        trim_for_pressure(policy, true);
        const auto& response = fill.head_;
        const auto control = cache_header(response.headers_, "Cache-Control");
        const auto content_encoding = cache_header(response.headers_, "Content-Encoding");
        if (!fill.active_ || fill.body_bytes_ < static_cast<std::size_t>(rule.min_object_bytes_) ||
            (!content_encoding.empty() && !flexedge::route_policy::ascii_equal(content_encoding, "identity") &&
                !flexedge::route_policy::ascii_equal(content_encoding, ticket.encoding_))) {
            abandon(ticket);
            return false;
        }
        auto item = std::make_shared<cached_response>();
        item->website_id_ = ticket.website_id_;
        item->policy_id_ = ticket.policy_id_;
        item->url_ = ticket.url_;
        item->website_epoch_ = ticket.website_epoch_;
        item->policy_epoch_ = ticket.policy_epoch_;
        item->body_bytes_ = fill.body_bytes_;
        if (!metadata(*item, response, rule, request, ticket.encoding_)) {
            fill.failure_reason_ = "origin_ttl";
            abandon(ticket);
            return false;
        }
        if (policy.storage() != "memory") {
            item->file_ = fill.finish_file();
            if (!item->file_) {
                abandon(ticket);
                return false;
            }
        }
        struct pending_metadata final {
            std::shared_ptr<const cache_file> file_;
            bool published_{};
            ~pending_metadata() {
                if (file_ && !published_) {
                    response_cache_publication::erase_metadata(*file_);
                }
            }
        } pending{item->file_};
        const auto limits = capacity_limits(policy, *publication_->snapshot());
        if (policy.storage() == "memory" && item->bytes_ > limits.memory_) {
            abandon(ticket);
            return false;
        }
        if (fill.retain_memory_ && (policy.storage() == "memory" || (policy.storage() == "tiered" && item->bytes_ <= limits.memory_))) {
            item->body_ = std::make_shared<const std::string>(std::move(fill.memory_));
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
            bool retire{};
            std::erase_if(variants, [&](const auto& existing) {
                if (existing->vary_ != item->vary_) {
                    return false;
                }
                retire |= static_cast<bool>(existing->file_);
                return true;
            });
            if (variants.size() >= 16) {
                retire |= static_cast<bool>(variants.front()->file_);
                variants.erase(variants.begin());
            }
            item->sequence_ = ++next->sequence_;
            item->identity_ = item->sequence_;
            variants.push_back(std::shared_ptr<const cached_response>(item));
            retire |= evict(*next, policy, item->stored_at_, capacity_limits(policy, *next));
            if (!next->entries_.contains(ticket.key_)) {
                abandon(ticket);
                return false;
            }
            if (!publication_->persist(ticket.key_, *item)) {
                abandon(ticket);
                return false;
            }
            const auto result = publication_->publish(root, std::shared_ptr<const response_cache_snapshot>(std::move(next)), retire);
            if (result) {
                pending.published_ = true;
                return true;
            }
        }
    }
    void discard(const std::string& key, std::uint64_t identity) {
        auto root = publication_->snapshot();
        for (;;) {
            const auto found = root->entries_.find(key);
            if (found == root->entries_.end()) {
                return;
            }
            auto next = std::make_shared<response_cache_snapshot>(*root);
            auto& entries = next->entries_.at(key);
            std::erase_if(entries, [&](const auto& item) { return item->identity_ == identity; });
            if (entries.empty()) {
                next->entries_.erase(key);
            }
            if (publication_->publish(root, std::shared_ptr<const response_cache_snapshot>(std::move(next)), true)) {
                return;
            }
        }
    }
    std::shared_ptr<const cached_response> revalidate(const cache_ticket& ticket, const flexedge::cache::rule& rule,
        const cached_response& previous, const cache_response_head& validation, const cache_headers& request) {
        const auto old_etag = cache_header(previous.headers_, "ETag");
        const auto new_etag = cache_header(validation.headers_, "ETag");
        const auto weak_value = [](std::string_view value) { return value.starts_with("W/") ? value.substr(2) : value; };
        if (!old_etag.empty() && !new_etag.empty() && weak_value(old_etag) != weak_value(new_etag)) {
            return {};
        }
        cache_response_head head{static_cast<std::uint16_t>(previous.status_), previous.headers_};
        std::erase_if(head.headers_, [](const auto& field) { return flexedge::route_policy::ascii_equal(field.first, "Date") || flexedge::route_policy::ascii_equal(field.first, "Age"); });
        for (const auto& field : validation.headers_) {
            if (flexedge::route_policy::ascii_equal(field.first, "Content-Encoding") && field.second != cache_header(previous.headers_, "Content-Encoding")) {
                return {};
            }
            std::erase_if(head.headers_, [&](const auto& old) { return flexedge::route_policy::ascii_equal(old.first, field.first); });
            auto value = field.second;
            if (flexedge::route_policy::ascii_equal(field.first, "ETag") && !cache_header(previous.headers_, "Content-Encoding").empty() && value.starts_with('"')) {
                value.insert(0, "W/");
            }
            head.headers_.emplace_back(field.first, std::move(value));
        }
        if (cache_header(head.headers_, "Date").empty()) {
            head.headers_.emplace_back("Date", cache_http_date(std::time(nullptr)));
        }
        auto refreshed = std::make_shared<cached_response>(previous);
        if (!metadata(*refreshed, head, rule, request, ticket.encoding_)) {
            discard(ticket.key_, previous.identity_);
            return {};
        }
        auto root = publication_->snapshot();
        for (;;) {
            const auto claim = root->filling_.find(ticket.key_);
            const auto found = root->entries_.find(ticket.key_);
            if (claim == root->filling_.end() || claim->second.id_ != ticket.claim_ || found == root->entries_.end() ||
                epoch(*root, "website:" + ticket.website_id_) != ticket.website_epoch_ || epoch(*root, "policy:" + ticket.policy_id_) != ticket.policy_epoch_) {
                return {};
            }
            const auto old = std::ranges::find_if(found->second, [&](const auto& item) { return item->identity_ == previous.identity_; });
            if (old == found->second.end()) {
                return {};
            }
            auto next = std::make_shared<response_cache_snapshot>(*root);
            refreshed->sequence_ = ++next->sequence_;
            auto& entries = next->entries_.at(ticket.key_);
            *std::ranges::find_if(entries, [&](const auto& item) { return item->identity_ == previous.identity_; }) = refreshed;
            next->filling_.erase(ticket.key_);
            if (!publication_->persist(ticket.key_, *refreshed)) {
                return {};
            }
            if (publication_->publish(root, std::shared_ptr<const response_cache_snapshot>(std::move(next)))) {
                return refreshed;
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
    std::pair<std::uint64_t, std::uint64_t> invalidate(const v2::CacheCommand& command,
        std::span<const cache_invalidation_url> urls) {
        auto root = publication_->snapshot();
        for (;;) {
            auto next = std::make_shared<response_cache_snapshot>(*root);
            if (command.operation() == "clean") {
                const auto key = "policy:" + command.policy_id();
                publication_->persist_epoch(key, ++next->epochs_[key]);
            } else {
                for (const auto& site : command.website_ids()) {
                    const auto key = "website:" + site;
                    publication_->persist_epoch(key, ++next->epochs_[key]);
                }
            }
            std::erase_if(next->filling_, [&](const auto& pair) {
                return pair.second.policy_id_ == command.policy_id() && (command.operation() == "clean" ||
                                                                            std::ranges::find(command.website_ids(), pair.second.website_id_) != command.website_ids().end());
            });
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
                        const auto [scheme, target] = cache_resource_address(item->url_);
                        for (const auto& url : urls) {
                            if (url.website_id_ != item->website_id_ || url.url_.scheme_ != scheme) {
                                continue;
                            }
                            std::string_view candidate = url.url_.target_;
                            if (item->ignore_query_) {
                                candidate = candidate.substr(0, candidate.find('?'));
                            }
                            if (command.scope() == "prefix" ? target.starts_with(candidate) : target == candidate) {
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
            // Preserve unrelated retained objects across the new invalidation fence.
            for (auto& [key, entries] : next->entries_) {
                for (auto& item : entries) {
                    const auto website_epoch = epoch(*next, "website:" + item->website_id_);
                    const auto policy_epoch = epoch(*next, "policy:" + item->policy_id_);
                    if (item->website_epoch_ == website_epoch && item->policy_epoch_ == policy_epoch) {
                        continue;
                    }
                    auto retained = std::make_shared<cached_response>(*item);
                    retained->website_epoch_ = website_epoch;
                    retained->policy_epoch_ = policy_epoch;
                    if (!publication_->persist(key, *retained)) {
                        throw std::runtime_error("cannot persist retained cache metadata");
                    }
                    item = std::shared_ptr<const cached_response>(std::move(retained));
                }
            }
            if (publication_->publish(root, std::shared_ptr<const response_cache_snapshot>(std::move(next)), true)) {
                return {count, bytes};
            }
        }
    }

private:
    std::shared_ptr<const cached_response> record_hit(const std::string& key,
        std::shared_ptr<const cached_response> object) {
        auto& ids = touches_[key];
        if (std::ranges::find(ids, object->identity_) == ids.end()) {
            ids.push_back(object->identity_);
        }
        if (++hits_since_flush_ >= 128) {
            flush_touches();
        }
        return object;
    }
    void flush_touches() {
        if (touches_.empty()) {
            return;
        }
        auto root = publication_->snapshot();
        for (;;) {
            auto next = std::make_shared<response_cache_snapshot>(*root);
            for (const auto& [key, identities] : touches_) {
                const auto found = next->entries_.find(key);
                if (found == next->entries_.end()) {
                    continue;
                }
                for (auto& item : found->second) {
                    if (std::ranges::find(identities, item->identity_) == identities.end()) {
                        continue;
                    }
                    auto touched = std::make_shared<cached_response>(*item);
                    touched->sequence_ = ++next->sequence_;
                    item = std::shared_ptr<const cached_response>(std::move(touched));
                }
            }
            if (publication_->publish(root, std::shared_ptr<const response_cache_snapshot>(std::move(next)))) {
                break;
            }
        }
        touches_.clear();
        hits_since_flush_ = 0;
    }
    static bool metadata(cached_response& item, const cache_response_head& response,
        const flexedge::cache::rule& rule, const cache_headers& request, const std::string& encoding) {
        const auto control = cache_header(response.headers_, "Cache-Control");
        if (!response_bypass_reason(rule, response).empty()) {
            return false;
        }
        const auto wall = std::time(nullptr);
        const auto date = cache_http_time(cache_header(response.headers_, "Date")).value_or(wall);
        const auto age = (std::max)(std::int64_t{0}, static_cast<std::int64_t>(wall - date));
        std::int64_t upstream_age{};
        const auto age_text = cache_header(response.headers_, "Age");
        if (!age_text.empty()) {
            const auto parsed = std::from_chars(age_text.data(), age_text.data() + age_text.size(), upstream_age);
            if (parsed.ec != std::errc{} || parsed.ptr != age_text.data() + age_text.size() || upstream_age < 0) {
                return false;
            }
        }
        item.initial_age_ = (std::max)(age, upstream_age);
        auto seconds = rule.ttl_seconds_;
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
        if (seconds <= item.initial_age_) {
            return false;
        }
        item.headers_ = response.headers_;
        item.status_ = response.status_;
        item.bytes_ = item.body_bytes_ + 256;
        for (const auto& field : item.headers_) {
            item.bytes_ += field.first.size() + field.second.size();
        }
        item.browser_ttl_seconds_ = rule.browser_ttl_seconds_;
        item.ignore_query_ = rule.ignore_query_;
        item.stored_at_ = std::chrono::steady_clock::now();
        item.expires_at_ = item.stored_at_ + std::chrono::seconds(seconds - item.initial_age_);
        item.stale_if_error_seconds_ = cache_directive_seconds(control, "stale-if-error").value_or(300);
        item.stale_if_error_seconds_ = (std::min)(item.stale_if_error_seconds_, std::int64_t{3600});
        if (cache_has_directive(control, "must-revalidate") || cache_has_directive(control, "proxy-revalidate") || cache_directive_seconds(control, "s-maxage").has_value()) {
            item.stale_if_error_seconds_ = 0;
        }
        const bool validator = !cache_header(item.headers_, "ETag").empty() || !cache_header(item.headers_, "Last-Modified").empty();
        item.retain_until_ = item.expires_at_ + std::chrono::seconds((std::max)(item.stale_if_error_seconds_, validator ? std::int64_t{86400} : std::int64_t{300}));
        item.vary_.clear();
        const auto vary = cache_tokens(cache_header(response.headers_, "Vary"));
        if (vary.size() > 16 || std::ranges::find(vary, "*") != vary.end()) {
            return false;
        }
        for (const auto& field : vary) {
            if (!flexedge::route_policy::http_token(field)) {
                return false;
            }
            item.vary_.emplace_back(field, field == "accept-encoding" ? encoding : cache_header(request, field));
        }
        return true;
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
            const auto retire = evict(*next, policy, now, limits);
            if (publication_->publish(root, std::shared_ptr<const response_cache_snapshot>(std::move(next)), retire)) {
                return;
            }
        }
    }

public:
    std::unique_ptr<cache_fill> begin_promotion(const cached_response& object, const v2::CachePolicy& policy,
        buffered_bytes_lease reservation) {
        if (policy.storage() != "tiered" || object.body_ || !object.file_ ||
            object.bytes_ > capacity_limits(policy, *publication_->snapshot()).memory_) {
            return {};
        }
        return std::make_unique<cache_fill>(cache_response_head{}, object.body_bytes_, object.body_bytes_, true, std::move(reservation));
    }
    void promote(const std::string& key, const std::shared_ptr<const cached_response>& object,
        const v2::CachePolicy& policy, cache_fill& fill) {
        if (!fill.active_ || fill.body_bytes_ != object->body_bytes_) {
            return;
        }
        flush_touches();
        const auto body = std::make_shared<const std::string>(std::move(fill.memory_));
        auto root = publication_->snapshot();
        for (;;) {
            const auto found = root->entries_.find(key);
            if (found == root->entries_.end() || object->expires_at_ <= std::chrono::steady_clock::now()) {
                return;
            }
            const auto current = std::ranges::find_if(found->second, [&](const auto& item) {
                return item->file_ == object->file_;
            });
            if (current == found->second.end() || (*current)->body_) {
                return;
            }
            auto replacement = std::make_shared<response_cache_snapshot>(*root);
            auto promoted = std::make_shared<cached_response>(**current);
            promoted->body_ = body;
            promoted->sequence_ = ++replacement->sequence_;
            auto& variants = replacement->entries_.at(key);
            *std::ranges::find(variants, *current) = std::shared_ptr<const cached_response>(promoted);
            const auto retire = evict(*replacement, policy, std::chrono::steady_clock::now(), capacity_limits(policy, *replacement));
            if (publication_->publish(root, std::shared_ptr<const response_cache_snapshot>(std::move(replacement)), retire)) {
                return;
            }
        }
    }

private:
    static bool evict(response_cache_snapshot& snapshot, const v2::CachePolicy& policy,
        std::chrono::steady_clock::time_point now, capacity limits) {
        // Demotion only releases a memory copy. Scan/retire disk metadata only
        // when a representation that owns a file actually leaves the root.
        bool retire{};
        std::vector<std::pair<std::string, std::shared_ptr<const cached_response>>> items;
        std::uint64_t memory{}, disk{};
        const auto memory_cost = [](const auto& item) { return item->bytes_ - (item->body_ ? 0 : item->body_bytes_); };
        for (auto at = snapshot.entries_.begin(); at != snapshot.entries_.end();) {
            std::erase_if(at->second, [&](const auto& item) {
                if (item->retain_until_ > now) {
                    return false;
                }
                retire |= static_cast<bool>(item->file_);
                return true;
            });
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
                retire = true;
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
        return retire;
    }
    std::shared_ptr<response_cache_publication> publication_;
    std::unordered_map<std::string, compiled_policy> policies_{};
    std::unordered_map<std::string, std::vector<std::uint64_t>> touches_{};
    std::size_t hits_since_flush_{};
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> pressure_checks_{};
};
}  // namespace flexedge::node
