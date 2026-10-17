#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "common/route_match.h"
#include "common/route_policy.h"
#include "node/proto/edge_control.pb.h"

namespace flexedge::node {

// Protocol limits of a published website. The control plane's input validation reports them
// as field errors; nodes reject a release that exceeds them.
inline constexpr std::size_t max_route_rules{100};
inline constexpr std::size_t max_route_conditions{20};
inline constexpr std::size_t max_route_hostnames{100};
inline constexpr std::size_t max_route_methods{8};
inline constexpr std::size_t max_route_headers{20};
inline constexpr std::size_t max_route_header_value_bytes{4096};
inline constexpr std::size_t max_route_path_bytes{2048};
inline constexpr std::size_t max_route_name_bytes{100};
inline constexpr std::size_t max_route_description_bytes{1000};
inline constexpr std::uint32_t min_response_compression_bytes{256};
inline constexpr std::uint32_t max_response_compression_min_bytes{1024 * 1024};
inline constexpr std::uint32_t max_response_compression_bytes{64 * 1024 * 1024};
inline constexpr std::size_t max_response_compression_match_values{32};
inline constexpr std::size_t max_response_compression_match_value_bytes{127};
inline constexpr std::size_t max_hostname_bytes{253};

class invalid_website final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Regex patterns of one website's route rules, compiled once while it is validated.
using route_patterns = std::unordered_map<const v2::RouteRule*, std::shared_ptr<const RE2>>;

[[nodiscard]] inline bool http_token(std::string_view value) noexcept {
    return flexedge::route_policy::http_token(value, value.size());
}

// A header a route rule may set: a token outside the connection-level and HSTS headers the
// proxy owns.
[[nodiscard]] inline bool route_header_name(std::string_view value) noexcept {
    namespace policy = flexedge::route_policy;
    return http_token(value) && !policy::ascii_equal(value, "Connection") &&
           !policy::ascii_equal(value, "Content-Length") && !policy::ascii_equal(value, "Host") &&
           !policy::ascii_equal(value, "Keep-Alive") &&
           !policy::ascii_equal(value, "Proxy-Connection") && !policy::ascii_equal(value, "TE") &&
           !policy::ascii_equal(value, "Strict-Transport-Security") &&
           !policy::ascii_equal(value, "Trailer") &&
           !policy::ascii_equal(value, "Transfer-Encoding") &&
           !policy::ascii_equal(value, "Upgrade");
}

[[nodiscard]] inline bool route_header_value(std::string_view value) noexcept {
    return value.size() <= max_route_header_value_bytes &&
           value.find('\r') == std::string_view::npos && value.find('\n') == std::string_view::npos;
}

[[nodiscard]] inline bool route_path(std::string_view value) noexcept {
    return !value.empty() && value.size() <= max_route_path_bytes && value.front() == '/' &&
           value.find('\r') == std::string_view::npos && value.find('\n') == std::string_view::npos;
}

[[nodiscard]] inline bool route_suffix(std::string_view value) {
    return !value.empty() && flexedge::route_policy::clean_text(value, max_route_path_bytes) &&
           value.find_first_of(" ?#") == std::string_view::npos;
}

[[nodiscard]] inline bool route_redirect_url(std::string_view value) noexcept {
    return !value.empty() && value.size() <= max_route_path_bytes &&
           (value.front() == '/' || value.starts_with("http://") ||
               value.starts_with("https://")) &&
           std::ranges::none_of(value, [](unsigned char ch) { return std::isspace(ch) != 0; });
}

[[nodiscard]] inline bool route_method(std::string_view value) noexcept {
    return value == "GET" || value == "HEAD" || value == "POST" || value == "PUT" ||
           value == "PATCH" || value == "DELETE" || value == "OPTIONS";
}

[[nodiscard]] inline bool compression_algorithm(std::string_view value) noexcept {
    return value == "br" || value == "zstd" || value == "gzip";
}

[[nodiscard]] inline bool compression_mime_type(std::string_view value) noexcept {
    const auto slash = value.find('/');
    if (slash == std::string_view::npos ||
        value.find('/', slash + 1) != std::string_view::npos) {
        return false;
    }
    const auto type = value.substr(0, slash);
    const auto subtype = value.substr(slash + 1);
    return flexedge::route_policy::http_token(type, type.size()) &&
           (subtype == "*" || flexedge::route_policy::http_token(subtype, subtype.size()));
}

enum class compression_match_kind { mime_type,
    extension };

[[nodiscard]] inline bool compression_match_value(std::string_view value,
    compression_match_kind kind) noexcept {
    return !value.empty() && value.size() <= max_response_compression_match_value_bytes &&
           std::ranges::none_of(value, [](unsigned char ch) { return std::isspace(ch) != 0; }) &&
           (kind == compression_match_kind::extension ? value.front() == '.'
                                                      : compression_mime_type(value));
}

[[nodiscard]] inline bool access_log_status_code_range(std::string_view value) noexcept {
    return value == "1xx" || value == "2xx" || value == "3xx" || value == "4xx" || value == "5xx";
}

inline void validate_access_log(const v2::Website& website) {
    std::unordered_set<std::string_view> ranges;
    for (const auto& value : website.access_log_status_code_ranges()) {
        if (!access_log_status_code_range(value) || !ranges.emplace(value).second) {
            throw invalid_website("invalid access log status code range");
        }
    }
}

template <typename values_type>
inline void validate_compression_match_values(const values_type& values,
    compression_match_kind kind) {
    if (static_cast<std::size_t>(values.size()) > max_response_compression_match_values) {
        throw invalid_website("too many response compression match values");
    }
    std::unordered_set<std::string_view> seen;
    for (const auto& value : values) {
        if (!compression_match_value(value, kind) || !seen.emplace(value).second) {
            throw invalid_website("invalid response compression match value");
        }
    }
}

inline void validate_response_compression(const v2::Website& website) {
    if (website.response_compression_enabled() &&
        (website.response_compression_min_bytes() < min_response_compression_bytes ||
            website.response_compression_min_bytes() > max_response_compression_min_bytes ||
            (website.response_compression_max_bytes() != 0 &&
                website.response_compression_max_bytes() <
                    website.response_compression_min_bytes()) ||
            website.response_compression_max_bytes() > max_response_compression_bytes ||
            website.response_compression_algorithms().empty())) {
        throw invalid_website("invalid response compression policy");
    }
    std::unordered_set<std::string_view> algorithms;
    for (const auto& algorithm : website.response_compression_algorithms()) {
        if (!compression_algorithm(algorithm) || !algorithms.emplace(algorithm).second) {
            throw invalid_website("invalid response compression algorithm");
        }
    }
    validate_compression_match_values(website.response_compression_mime_types(),
        compression_match_kind::mime_type);
    validate_compression_match_values(website.response_compression_extensions(),
        compression_match_kind::extension);
    validate_compression_match_values(website.response_compression_excluded_extensions(),
        compression_match_kind::extension);
}

inline void validate_website_basics(const v2::Website& website) {
    if (website.id().empty() || website.revision() <= 0 || website.domains().empty()) {
        throw invalid_website("invalid enabled website");
    }
    if (website.origin_connect_timeout_seconds() == 0 ||
        website.origin_read_timeout_seconds() == 0) {
        throw invalid_website("enabled website requires finite origin timeouts");
    }
    if (website.health_check_enabled() &&
        (website.health_check_path().empty() || website.health_check_path().front() != '/' ||
            website.health_check_interval_seconds() == 0 ||
            website.health_check_timeout_seconds() == 0 ||
            (website.health_check_expected_status() != 0 &&
                website.health_check_expected_status() < 100) ||
            website.health_check_expected_status() > 599 || website.healthy_threshold() == 0 ||
            website.unhealthy_threshold() == 0)) {
        throw invalid_website("invalid origin health check policy");
    }
    if (website.force_https() && !website.https_enabled()) {
        throw invalid_website("force_https requires https_enabled");
    }
    if (website.hsts_enabled() && !website.https_enabled()) {
        throw invalid_website("HTTPS feature requires https_enabled");
    }
    if (website.https_enabled() && website.minimum_tls_version() != "1.2" &&
        website.minimum_tls_version() != "1.3") {
        throw invalid_website("unsupported minimum TLS version");
    }
    validate_access_log(website);
    validate_response_compression(website);
}

inline bool route_methods(const google::protobuf::RepeatedPtrField<std::string>& methods) {
    if (static_cast<std::size_t>(methods.size()) > max_route_methods) {
        return false;
    }
    std::unordered_set<std::string_view> seen;
    for (const auto& method : methods) {
        if (!route_method(method) || !seen.emplace(method).second) {
            return false;
        }
    }
    return true;
}

inline bool route_headers(const google::protobuf::RepeatedPtrField<v2::HeaderMutation>& headers) {
    if (static_cast<std::size_t>(headers.size()) > max_route_headers) {
        return false;
    }
    std::unordered_set<std::string> names;
    for (const auto& header : headers) {
        if (!route_header_name(header.name()) || !route_header_value(header.value())) {
            return false;
        }
        std::string normalized(header.name());
        std::ranges::transform(normalized, normalized.begin(),
            [](unsigned char ch) { return flexedge::route_policy::lower_ascii(ch); });
        if (!names.emplace(std::move(normalized)).second) {
            return false;
        }
    }
    return true;
}

inline bool route_origin_ids(const google::protobuf::RepeatedPtrField<std::string>& ids,
    const v2::Website& website) {
    if (ids.size() > 20) {
        return false;
    }
    std::unordered_set<std::string_view> selected;
    std::unordered_set<std::string_view> enabled;
    for (const auto& origin : website.origins()) {
        if (origin.enabled()) {
            enabled.emplace(origin.id());
        }
    }
    for (const auto& id : ids) {
        if (id.empty() || !selected.emplace(id).second || !enabled.contains(id)) {
            return false;
        }
    }
    return true;
}

inline bool route_origin_group(const v2::RouteRule& rule, const v2::Website& website) {
    if (!rule.origin_ids().empty()) {
        return route_origin_ids(rule.origin_ids(), website);
    }
    if (rule.origin_group().empty()) {
        return false;
    }
    for (const auto& origin : website.origins()) {
        const auto group =
            origin.group().empty() ? std::string_view{"default"} : std::string_view(origin.group());
        if (group == rule.origin_group() && origin.enabled() && origin.role() == "primary") {
            return true;
        }
    }
    return false;
}

// Regex patterns compiled on the way are kept in `patterns` when given.
inline void validate_route_rules(const v2::Website& website, route_patterns* patterns = nullptr) {
    if (static_cast<std::size_t>(website.route_rules_size()) > max_route_rules) {
        throw invalid_website("too many website route rules");
    }
    std::unordered_set<std::string_view> ids;
    for (const auto& rule : website.route_rules()) {
        namespace policy = flexedge::route_policy;
        if (rule.match_type() == "regex") {
            auto pattern = policy::compile_pattern(rule.path());
            if (!pattern ||
                !policy::capture_template(rule.rewrite_path(), pattern->NumberOfCapturingGroups()) ||
                !policy::capture_template(rule.redirect_url(), pattern->NumberOfCapturingGroups())) {
                throw invalid_website("invalid route regex or capture template");
            }
            if (patterns) {
                patterns->emplace(&rule, std::move(pattern));
            }
        }
        if (static_cast<std::size_t>(rule.conditions_size()) > max_route_conditions) {
            throw invalid_website("too many route conditions");
        }
        for (const auto& condition : rule.conditions()) {
            if (!policy::condition_options(condition.source(), condition.name(), condition.op(),
                    condition.value())) {
                throw invalid_website("invalid route condition");
            }
        }
        const auto& mode = rule.rewrite_mode();
        if (!policy::clean_text(rule.name(), max_route_name_bytes) ||
            rule.description().size() > max_route_description_bytes ||
            static_cast<std::size_t>(rule.hostnames_size()) > max_route_hostnames ||
            !policy::rewrite_mode(mode) || !policy::query_mode(rule.query_mode()) ||
            !policy::query_string(rule.query_string()) ||
            (rule.query_mode() != "replace" && !rule.query_string().empty()) ||
            ((mode == "none" || mode == "strip_prefix") && !rule.rewrite_path().empty()) ||
            ((mode == "replace_path" || mode == "replace_prefix") && rule.rewrite_path().empty()) ||
            ((mode == "strip_prefix" || mode == "replace_prefix") &&
                (rule.match_type() != "prefix" || !policy::path_only(rule.path()) ||
                    (mode == "replace_prefix" && !policy::path_only(rule.rewrite_path())))) ||
            (rule.action() == "redirect" && !mode.empty() && mode != "none")) {
            throw invalid_website("invalid route policy options");
        }
        std::unordered_set<std::string> hostnames;
        for (const auto& host : rule.hostnames()) {
            if (!policy::exact_hostname(host) || !hostnames.emplace(policy::hostname(host)).second) {
                throw invalid_website("invalid route hostname filter");
            }
        }
        if (rule.id().empty() || !ids.emplace(rule.id()).second ||
            !policy::valid_match_type(rule.match_type()) ||
            (rule.match_type() == "suffix"
                    ? !route_suffix(rule.path())
                    : (rule.match_type() != "regex" && !route_path(rule.path()))) ||
            !route_methods(rule.methods()) ||
            (rule.action() != "proxy" && rule.action() != "redirect" && rule.action() != "rewrite") ||
            !route_headers(rule.request_headers()) || !route_headers(rule.response_headers())) {
            throw invalid_website("invalid website route rule");
        }
        if (rule.action() == "rewrite") {
            if (!rule.origin_ids().empty() || !rule.origin_group().empty() ||
                !rule.redirect_url().empty() || rule.redirect_status() != 0 ||
                !rule.request_headers().empty() || !rule.response_headers().empty() ||
                (!rule.rewrite_path().empty() && !policy::path_only(rule.rewrite_path()))) {
                throw invalid_website("invalid rewrite route rule");
            }
        } else if (rule.action() == "proxy") {
            if (!route_origin_group(rule, website) ||
                (!rule.rewrite_path().empty() && !policy::path_only(rule.rewrite_path())) ||
                !rule.redirect_url().empty() || rule.redirect_status() != 0) {
                throw invalid_website("invalid proxy route rule");
            }
        } else if (!rule.origin_ids().empty() || !rule.origin_group().empty() ||
                   !rule.rewrite_path().empty() || !route_redirect_url(rule.redirect_url()) ||
                   !policy::redirect_status(rule.redirect_status())) {
            throw invalid_website("invalid redirect route rule");
        }
    }
}

inline void validate_website_origins(const v2::Website& website) {
    std::unordered_set<std::string_view> enabled_groups;
    std::unordered_set<std::string_view> primary_groups;
    for (const auto& origin : website.origins()) {
        if (!origin.enabled()) {
            continue;
        }
        if (origin.id().empty() || origin.host().empty() || origin.port() == 0 ||
            origin.port() > 65535 ||
            (origin.protocol() != "http" && origin.protocol() != "https") ||
            (origin.role() != "primary" && origin.role() != "backup") || origin.weight() == 0) {
            throw invalid_website("invalid enabled website origin");
        }
        const auto group = origin.group().empty() ? std::string_view{"default"}
                                                  : std::string_view(origin.group());
        enabled_groups.emplace(group);
        if (origin.role() == "primary") {
            primary_groups.emplace(group);
        }
    }
    const auto default_group = website.default_origin_group().empty()
                                   ? std::string_view{"default"}
                                   : std::string_view(website.default_origin_group());
    if (enabled_groups.empty() || enabled_groups != primary_groups ||
        !enabled_groups.contains(default_group)) {
        throw invalid_website("enabled website requires an enabled origin");
    }
}

// Certificate material and hostname uniqueness span the whole release; the node checks them
// when it indexes the release.
inline void validate_website_domains(const v2::Website& website) {
    bool has_https_domain = false;
    for (const auto& domain : website.domains()) {
        const auto hostname = flexedge::route_policy::hostname(domain.hostname());
        if (hostname.empty() || hostname.size() > max_hostname_bytes) {
            throw invalid_website("hostname is empty or exceeds 253 bytes after normalization");
        }
        if (domain.https_enabled() && !website.https_enabled()) {
            throw invalid_website("HTTPS domain requires HTTPS website");
        }
        if (domain.https_enabled() && domain.certificate_digest().empty()) {
            throw invalid_website("HTTPS domain requires certificate material");
        }
        has_https_domain = has_https_domain || domain.https_enabled();
    }
    if (website.https_enabled() && !has_https_domain) {
        throw invalid_website("HTTPS website requires an HTTPS domain");
    }
}

// The protocol rules of one published website, checked by the control plane before it
// publishes and by the node before it serves. Disabled websites are delivered but never
// served, so they are not checked.
inline void validate_website(const v2::Website& website, route_patterns* patterns = nullptr) {
    if (!website.enabled()) {
        return;
    }
    validate_website_basics(website);
    validate_route_rules(website, patterns);
    validate_website_origins(website);
    validate_website_domains(website);
}

}  // namespace flexedge::node
