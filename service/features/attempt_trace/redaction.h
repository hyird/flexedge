#pragma once

#include <ruvia/web/model.h>
#include <ruvia/web/model_object.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include "service/features/attempt_trace/model.h"
#include "service/features/attempt_trace/outbound_policy.h"
#include "service/utils/sensitive_string.h"
#include "service/utils/utf8.h"

// The only way text reaches an attempt trace. Three layers apply to every
// stored string: the per-operation allow-lists (outbound_policy.h), exact
// replacement of every secret this attempt opened or sent in any common
// encoding, and pattern scans for key material, bearer tokens, JWTs and
// credential-looking `name=value` pairs.
namespace service::attempt_trace {

class redactor;

// Text produced by the redactor; the trace model stores nothing else.
class redacted_text final {
public:
    redacted_text() = default;

    [[nodiscard]] std::string_view view() const noexcept {
        return value_;
    }

    [[nodiscard]] bool empty() const noexcept {
        return value_.empty();
    }

    friend bool operator==(const redacted_text&, const redacted_text&) = default;

private:
    explicit redacted_text(std::string value) noexcept
        : value_(std::move(value)) {}

    std::string value_;
    friend class redactor;
};

struct redacted_pair final {
    redacted_text name_;
    redacted_text value_;
    bool redacted_{};
};

// `omitted_reason_` empty means the excerpt is present.
struct redacted_body final {
    redacted_text content_type_;
    std::string_view format_;
    std::int64_t bytes_{};
    redacted_text excerpt_;
    bool truncated_{};
    std::string_view omitted_reason_;
};

struct redacted_url final {
    redacted_text url_;
    redacted_text path_;
    std::vector<redacted_pair> query_;
};

struct dns_answer final {
    redacted_text name_;
    redacted_text type_;
    std::int64_t ttl_{};
    redacted_text data_;
};

struct dns_observation final {
    redacted_text name_;
    redacted_text type_;
    std::string rcode_;
    std::string rcode_number_;
    std::vector<dns_answer> answers_;
    bool truncated_{};
};

struct response_facts final {
    redacted_text request_id_;
    redacted_text provider_code_;
    std::optional<dns_observation> dns_;
};

struct header_entry final {
    std::string_view name_;
    std::string_view value_;
};

namespace text_detail {

[[nodiscard]] inline char lower_ascii(char value) noexcept {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
}

[[nodiscard]] inline bool equals_ignore_case(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (lower_ascii(left[index]) != lower_ascii(right[index])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool matches_ignore_case_at(std::string_view value, std::size_t position,
    std::string_view needle) noexcept {
    return position + needle.size() <= value.size() && equals_ignore_case(value.substr(position, needle.size()), needle);
}

[[nodiscard]] inline std::string lower(std::string_view value) {
    std::string result(value);
    for (auto& ch : result) {
        ch = lower_ascii(ch);
    }
    return result;
}

[[nodiscard]] inline bool contains(std::span<const std::string_view> values, std::string_view value) noexcept {
    return std::find(values.begin(), values.end(), value) != values.end();
}

[[nodiscard]] inline bool is_alnum(char value) noexcept {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9');
}

[[nodiscard]] inline bool is_base64_url(char value) noexcept {
    return is_alnum(value) || value == '-' || value == '_';
}

// Valid UTF-8 only; NUL and control characters become spaces except line
// feeds and tabs in multi-line text (PostgreSQL rejects NUL in jsonb).
[[nodiscard]] inline std::string sanitized(std::string_view value, bool multiline) {
    std::string result;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size();) {
        const auto length = service::utils::utf8_sequence_length(value, index);
        if (length == 0) {
            ++index;
            continue;
        }
        const auto lead = static_cast<unsigned char>(value[index]);
        if (length == 1 && (lead < 0x20 || lead == 0x7F)) {
            if (lead == '\r') {
                if (!multiline) {
                    result.push_back(' ');
                }
            } else if (multiline && (lead == '\n' || lead == '\t')) {
                result.push_back(static_cast<char>(lead));
            } else {
                result.push_back(' ');
            }
        } else {
            result.append(value.substr(index, length));
        }
        index += length;
    }
    return result;
}

[[nodiscard]] inline bool unreserved(unsigned char value) noexcept {
    return is_alnum(static_cast<char>(value)) || value == '-' || value == '.' || value == '_' || value == '~';
}

[[nodiscard]] inline std::string percent_encode(std::string_view value, bool upper_hex, bool form) {
    constexpr std::string_view upper_digits = "0123456789ABCDEF";
    constexpr std::string_view lower_digits = "0123456789abcdef";
    const auto digits = upper_hex ? upper_digits : lower_digits;
    std::string result;
    result.reserve(value.size() * 3);
    for (const auto raw : value) {
        const auto ch = static_cast<unsigned char>(raw);
        if (unreserved(ch)) {
            result.push_back(raw);
        } else if (form && ch == ' ') {
            result.push_back('+');
        } else {
            result.push_back('%');
            result.push_back(digits[ch >> 4]);
            result.push_back(digits[ch & 0x0F]);
        }
    }
    return result;
}

[[nodiscard]] inline int hex_value(char value) noexcept {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

// Decodes %XX (and '+' as space); malformed escapes are kept literally.
[[nodiscard]] inline std::string percent_decode(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] == '%' && index + 2 < value.size()) {
            const auto high = hex_value(value[index + 1]);
            const auto low = hex_value(value[index + 2]);
            if (high >= 0 && low >= 0) {
                result.push_back(static_cast<char>(high * 16 + low));
                index += 2;
                continue;
            }
        }
        result.push_back(value[index] == '+' ? ' ' : value[index]);
    }
    return result;
}

[[nodiscard]] inline std::string base64_encode(std::string_view value, bool url) {
    constexpr std::string_view standard = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    constexpr std::string_view url_safe = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    const auto alphabet = url ? url_safe : standard;
    std::string result;
    result.reserve((value.size() + 2) / 3 * 4);
    std::size_t index = 0;
    for (; index + 2 < value.size(); index += 3) {
        const auto chunk = (static_cast<unsigned char>(value[index]) << 16) |
                           (static_cast<unsigned char>(value[index + 1]) << 8) |
                           static_cast<unsigned char>(value[index + 2]);
        result.push_back(alphabet[(chunk >> 18) & 0x3F]);
        result.push_back(alphabet[(chunk >> 12) & 0x3F]);
        result.push_back(alphabet[(chunk >> 6) & 0x3F]);
        result.push_back(alphabet[chunk & 0x3F]);
    }
    const auto rest = value.size() - index;
    if (rest > 0) {
        auto chunk = static_cast<unsigned>(static_cast<unsigned char>(value[index])) << 16;
        if (rest == 2) {
            chunk |= static_cast<unsigned>(static_cast<unsigned char>(value[index + 1])) << 8;
        }
        result.push_back(alphabet[(chunk >> 18) & 0x3F]);
        result.push_back(alphabet[(chunk >> 12) & 0x3F]);
        if (rest == 2) {
            result.push_back(alphabet[(chunk >> 6) & 0x3F]);
        }
        if (!url) {
            result.append(rest == 1 ? "==" : "=");
        }
    }
    return result;
}

[[nodiscard]] inline std::optional<std::string> base64_url_decode(std::string_view value) {
    std::string result;
    result.reserve(value.size() * 3 / 4);
    unsigned buffer = 0;
    int bits = 0;
    for (const auto ch : value) {
        int digit = -1;
        if (ch >= 'A' && ch <= 'Z') {
            digit = ch - 'A';
        } else if (ch >= 'a' && ch <= 'z') {
            digit = ch - 'a' + 26;
        } else if (ch >= '0' && ch <= '9') {
            digit = ch - '0' + 52;
        } else if (ch == '-' || ch == '+') {
            digit = 62;
        } else if (ch == '_' || ch == '/') {
            digit = 63;
        } else if (ch == '=') {
            break;
        } else {
            return std::nullopt;
        }
        buffer = (buffer << 6) | static_cast<unsigned>(digit);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            result.push_back(static_cast<char>((buffer >> bits) & 0xFF));
        }
    }
    return result;
}

inline void append_json_string(std::string& output, std::string_view value) {
    constexpr std::string_view digits = "0123456789abcdef";
    output.push_back('"');
    for (const auto raw : value) {
        const auto ch = static_cast<unsigned char>(raw);
        switch (raw) {
            case '"':
                output.append("\\\"");
                break;
            case '\\':
                output.append("\\\\");
                break;
            case '\n':
                output.append("\\n");
                break;
            case '\t':
                output.append("\\t");
                break;
            case '\r':
                output.append("\\r");
                break;
            default:
                if (ch < 0x20) {
                    output.append("\\u00");
                    output.push_back(digits[ch >> 4]);
                    output.push_back(digits[ch & 0x0F]);
                } else {
                    output.push_back(raw);
                }
        }
    }
    output.push_back('"');
}

// Upper bound of the bytes `value` takes as a JSON string, quotes included.
[[nodiscard]] inline std::size_t json_string_size(std::string_view value) noexcept {
    std::size_t size = 2;
    for (const auto raw : value) {
        const auto ch = static_cast<unsigned char>(raw);
        if (raw == '"' || raw == '\\' || raw == '\n' || raw == '\t' || raw == '\r') {
            size += 2;
        } else if (ch < 0x20) {
            size += 6;
        } else {
            ++size;
        }
    }
    return size;
}

// Lower-case media type without parameters.
[[nodiscard]] inline std::string media_type(std::string_view content_type) {
    const auto end = content_type.find(';');
    auto type = content_type.substr(0, end);
    while (!type.empty() && (type.front() == ' ' || type.front() == '\t')) {
        type.remove_prefix(1);
    }
    while (!type.empty() && (type.back() == ' ' || type.back() == '\t')) {
        type.remove_suffix(1);
    }
    return lower(type);
}

[[nodiscard]] inline bool json_media_type(std::string_view type) noexcept {
    return type == "application/json" || type.ends_with("+json") || type == "application/dns-json" ||
           type == "text/json";
}

[[nodiscard]] inline std::string_view dns_type_name(std::int64_t value) noexcept {
    switch (value) {
        case 1:
            return "A";
        case 2:
            return "NS";
        case 5:
            return "CNAME";
        case 6:
            return "SOA";
        case 12:
            return "PTR";
        case 15:
            return "MX";
        case 16:
            return "TXT";
        case 28:
            return "AAAA";
        case 33:
            return "SRV";
        case 64:
            return "SVCB";
        case 65:
            return "HTTPS";
        case 257:
            return "CAA";
        default:
            return {};
    }
}

[[nodiscard]] inline std::string_view dns_rcode_name(std::int64_t value) noexcept {
    switch (value) {
        case 0:
            return "NOERROR";
        case 1:
            return "FORMERR";
        case 2:
            return "SERVFAIL";
        case 3:
            return "NXDOMAIN";
        case 4:
            return "NOTIMP";
        case 5:
            return "REFUSED";
        default:
            return {};
    }
}

inline constexpr std::array sensitive_parameter_names{std::string_view{"accesskeyid"},
    std::string_view{"accesskeysecret"}, std::string_view{"signaturenonce"}, std::string_view{"signature"},
    std::string_view{"access_key"}, std::string_view{"api_key"}, std::string_view{"apikey"},
    std::string_view{"eab_hmac_key"}, std::string_view{"hmac_key"}, std::string_view{"private_key"},
    std::string_view{"password"}, std::string_view{"secret"}, std::string_view{"token"}};

// Phrases after which a provider echoes the material it signed, e.g.
// Aliyun `server string to sign is:GET&%2F&AccessKeyId%3D…` or the
// `<StringToSign>`/`<CanonicalRequest>` elements of S3-style errors.
inline constexpr std::array signed_string_phrases{std::string_view{"string to sign"},
    std::string_view{"string-to-sign"}, std::string_view{"stringtosign"}, std::string_view{"canonical request"},
    std::string_view{"canonicalrequest"}};

}  // namespace text_detail

class redactor final {
public:
    redactor() = default;
    redactor(const redactor&) = delete;
    redactor& operator=(const redactor&) = delete;

    // Remembers a credential this attempt holds (token, key, account
    // identifier, e-mail): every stored or outgoing string has it replaced in
    // raw, percent-, form-, base64-, base64url- and JSON-encoded form. Values
    // shorter than 8 bytes are protected by the allow-lists only. Credentials
    // do not count against the cap of values observed in traffic; a
    // credential that could not be remembered makes external_text() return
    // its fallback.
    void register_secret(std::string_view value) noexcept {
        try {
            if (value.size() < minimum_secret_bytes || known(value)) {
                return;
            }
            if (secrets_.size() >= maximum_secrets + maximum_credential_variants) {
                secrets_incomplete_ = true;
                return;
            }
            add_secret(value);
        } catch (...) {
            failed_ = true;
            secrets_incomplete_ = true;
        }
    }

    // Text leaving the attempt: the task error, sys_sync_event.error, a
    // resource's last error and the server log. Same secret and pattern
    // scrub as stored trace text, without its length limit and without
    // counting towards capture.redactions. When a credential could not be
    // remembered, or the scrub fails, `fallback` replaces the whole text.
    [[nodiscard]] std::string external_text(std::string_view value, std::string_view fallback) noexcept {
        const auto counted = redactions_;
        try {
            if (secrets_incomplete_) {
                return std::string(fallback);
            }
            bool private_key = false;
            auto result = scrub(std::string(value), private_key);
            redactions_ = counted;
            return result;
        } catch (...) {
            failed_ = true;
            redactions_ = counted;
            try {
                return std::string(fallback);
            } catch (...) {
                return {};
            }
        }
    }

    // Single-line text: control characters become spaces.
    [[nodiscard]] redacted_text text(std::string_view value, std::size_t limit) noexcept {
        bool truncated = false;
        return text(value, limit, false, truncated);
    }

    [[nodiscard]] redacted_text text(std::string_view value, std::size_t limit, bool multiline,
        bool& truncated) noexcept {
        try {
            bool private_key = false;
            auto result = scrub(text_detail::sanitized(value, multiline), private_key);
            truncated = service::utils::truncate_utf8(result, limit) || truncated;
            return redacted_text(std::move(result));
        } catch (...) {
            failed_ = true;
            return redacted_text(std::string(redacted_marker));
        }
    }

    [[nodiscard]] redacted_url url(const outbound_policy& policy, std::string_view scheme, std::string_view host,
        std::string_view target) noexcept {
        redacted_url result;
        try {
            auto path = target.substr(0, target.find_first_of("?#"));
            auto query = std::string_view{};
            if (const auto question = target.find('?'); question != std::string_view::npos) {
                query = target.substr(question + 1);
                query = query.substr(0, query.find('#'));
            }
            std::string masked_path;
            bool mask_next = false;
            std::size_t start = 0;
            while (start <= path.size()) {
                const auto end = std::min(path.find('/', start), path.size());
                const auto segment = path.substr(start, end - start);
                if (mask_next && !segment.empty()) {
                    remember(segment);
                    masked_path.append(redacted_marker);
                    ++redactions_;
                } else {
                    masked_path.append(segment);
                }
                mask_next = text_detail::contains(policy.masked_path_after_, segment);
                if (end == path.size()) {
                    break;
                }
                masked_path.push_back('/');
                start = end + 1;
            }
            std::string rebuilt_query;
            std::size_t parameters = 0;
            start = 0;
            while (start < query.size()) {
                const auto end = std::min(query.find('&', start), query.size());
                const auto part = query.substr(start, end - start);
                start = end + 1;
                if (part.empty()) {
                    continue;
                }
                const auto equals = part.find('=');
                const auto raw_name = part.substr(0, equals);
                const auto raw_value = equals == std::string_view::npos ? std::string_view{} : part.substr(equals + 1);
                const auto name = text_detail::percent_decode(raw_name);
                const auto value = text_detail::percent_decode(raw_value);
                const bool allowed = text_detail::contains(policy.query_, name);
                if (!allowed) {
                    remember(value);
                    remember(raw_value);
                    ++redactions_;
                }
                if (!rebuilt_query.empty()) {
                    rebuilt_query.push_back('&');
                }
                rebuilt_query.append(raw_name);
                if (equals != std::string_view::npos) {
                    rebuilt_query.push_back('=');
                    rebuilt_query.append(allowed ? raw_value : redacted_marker);
                }
                if (parameters++ < max_query_parameters) {
                    result.query_.push_back({.name_ = text(name, value_bytes),
                        .value_ = allowed ? text(value, value_bytes) : redacted_text(std::string(redacted_marker)),
                        .redacted_ = !allowed});
                }
            }
            result.path_ = text(masked_path, url_bytes);
            std::string url(scheme);
            url.append("://").append(host).append(masked_path);
            if (!rebuilt_query.empty()) {
                url.push_back('?');
                url.append(rebuilt_query);
            }
            result.url_ = text(url, url_bytes);
        } catch (...) {
            failed_ = true;
            result = redacted_url{.url_ = redacted_text(std::string(redacted_marker)),
                .path_ = redacted_text(std::string(redacted_marker)),
                .query_ = {}};
        }
        return result;
    }

    // Request headers are written by FlexEdge itself; every value outside the
    // allow-list (authorization) is a credential and becomes a known secret.
    [[nodiscard]] redacted_pair request_header(std::string_view name, std::string_view value) noexcept {
        try {
            const auto lowered = text_detail::lower(name);
            if (text_detail::contains(request_header_allow_list, lowered)) {
                return {.name_ = text(lowered, value_bytes), .value_ = text(value, value_bytes), .redacted_ = false};
            }
            register_secret(value);
            if (const auto separator = value.find(' '); separator != std::string_view::npos) {
                register_secret(value.substr(separator + 1));
            }
            ++redactions_;
            return {.name_ = text(lowered, value_bytes), .value_ = marker(), .redacted_ = true};
        } catch (...) {
            failed_ = true;
            return {.name_ = marker(), .value_ = marker(), .redacted_ = true};
        }
    }

    // Response header values outside the allow-list are hidden; values of
    // credential-bearing headers (cookies, nonces, tokens) also become known
    // secrets so an echo elsewhere is hidden too.
    [[nodiscard]] redacted_pair response_header(std::string_view name, std::string_view value) noexcept {
        try {
            const auto lowered = text_detail::lower(name);
            if (text_detail::contains(response_header_allow_list, lowered)) {
                return {.name_ = text(lowered, value_bytes), .value_ = text(value, value_bytes), .redacted_ = false};
            }
            if (lowered == "set-cookie" || lowered == "replay-nonce" ||
                lowered.find("token") != std::string::npos || lowered.find("secret") != std::string::npos ||
                lowered.find("auth") != std::string::npos || lowered.find("key") != std::string::npos) {
                remember(value);
            }
            ++redactions_;
            return {.name_ = text(lowered, value_bytes), .value_ = marker(), .redacted_ = true};
        } catch (...) {
            failed_ = true;
            return {.name_ = marker(), .value_ = marker(), .redacted_ = true};
        }
    }

    // `limit` is the excerpt cap and `budget` what is left of the attempt's
    // excerpt budget; the excerpt is shortened to the smaller one.
    [[nodiscard]] redacted_body body(const outbound_policy& policy, bool request, std::string_view content_type,
        std::string_view body, std::size_t limit, std::size_t budget) noexcept {
        redacted_body result;
        result.bytes_ = static_cast<std::int64_t>(body.size());
        try {
            const auto type = text_detail::media_type(content_type);
            result.content_type_ = text(type, value_bytes);
            const auto rule = request ? policy.request_body_ : policy.response_body_;
            if (contains_private_key(body)) {
                ++redactions_;
                return omit(std::move(result), "secret_material");
            }
            if (rule == body_rule::none) {
                return omit(std::move(result), "policy");
            }
            if (body.size() > parse_body_max_bytes) {
                return omit(std::move(result), "too_large");
            }
            if (budget == 0) {
                return omit(std::move(result), "budget");
            }
            items_omitted_ = false;
            std::optional<std::string> excerpt;
            std::string_view format;
            const bool looks_json = !body.empty() && (body.front() == '{' || body.front() == '[');
            if (rule == body_rule::jws && type == "application/jose+json") {
                excerpt = jws_view(policy, body);
                format = "jws_decoded";
            } else if (rule == body_rule::form && type == "application/x-www-form-urlencoded") {
                excerpt = form_view(policy, body);
                format = "form";
            } else if (rule == body_rule::pem_chain && !request &&
                       (type == "application/pem-certificate-chain" || body.starts_with("-----BEGIN"))) {
                excerpt = certificate_chain_view(body);
                format = "pem_summary";
            } else if (text_detail::json_media_type(type) || (type.empty() && looks_json)) {
                const auto fields = request ? policy.request_fields_ : policy.response_fields_;
                excerpt = json_view(body, fields, request ? policy.request_sized_fields_ : policy.request_sized_fields_.first(0));
                format = "json";
            }
            if (!excerpt) {
                // JSON that does not parse is kept as scanned text; a JWS or a
                // certificate chain that does not parse is never kept raw.
                const bool json_attempted = format == "json";
                if (type.starts_with("text/") || ((type.empty() || json_attempted) && valid_text(body))) {
                    excerpt = std::string(body);
                    format = "text";
                } else {
                    return omit(std::move(result), "binary");
                }
            }
            // Array items beyond the item cap are summarized as "… N more".
            bool truncated = std::exchange(items_omitted_, false);
            result.excerpt_ = text(*excerpt, std::min(limit, budget), true, truncated);
            result.truncated_ = truncated;
            result.format_ = format;
        } catch (...) {
            failed_ = true;
            return omit(std::move(result), "policy");
        }
        return result;
    }

    // Provider request id, provider error code and, for DNS over HTTPS, the
    // query and its answers.
    [[nodiscard]] response_facts response(const outbound_policy& policy, std::span<const header_entry> headers,
        std::string_view content_type, std::string_view body, int status, std::string_view query_name,
        std::string_view query_type) noexcept {
        response_facts facts;
        try {
            for (const auto name : {std::string_view{"cf-ray"}, std::string_view{"x-acs-request-id"},
                     std::string_view{"x-request-id"}}) {
                for (const auto& header : headers) {
                    if (facts.request_id_.empty() && text_detail::equals_ignore_case(header.name_, name)) {
                        facts.request_id_ = text(header.value_, identifier_bytes);
                    }
                }
            }
            if (body.empty() || body.size() > parse_body_max_bytes || contains_private_key(body) ||
                (body.front() != '{' && body.front() != '[')) {
                return facts;
            }
            std::pmr::monotonic_buffer_resource pool;
            const auto document = ruvia::json_value::parse(body, {.resource_ = &pool});
            if (!document || !document->is_object()) {
                return facts;
            }
            if (facts.request_id_.empty()) {
                if (const auto value = document->get<ruvia::string>("RequestId"); value) {
                    facts.request_id_ = text(value->view(), identifier_bytes);
                }
            }
            const auto type = text_detail::media_type(content_type);
            if (const auto code = document->get<ruvia::string>("Code"); code) {
                facts.provider_code_ = text(code->view(), identifier_bytes);
            } else if (status >= 400 || type == "application/problem+json") {
                if (const auto problem = document->get<ruvia::string>("type"); problem) {
                    facts.provider_code_ = text(problem->view(), identifier_bytes);
                } else if (const auto error = document->get<ruvia::json_object>("error"); error) {
                    if (const auto error_type = error->get<ruvia::string>("type"); error_type) {
                        facts.provider_code_ = text(error_type->view(), identifier_bytes);
                    }
                }
            }
            if (facts.provider_code_.empty()) {
                if (const auto errors = document->get<ruvia::json_value>("errors"); errors && errors->is_array()) {
                    (void)errors->for_each_element([&](const ruvia::json_value& error) {
                        if (const auto code = error.get<ruvia::json_value>("code"); code) {
                            facts.provider_code_ = text(code->is_string() ? unquoted(*code) : code->view(), identifier_bytes);
                        }
                        return false;
                    });
                }
            }
            if (policy.dns_query_) {
                facts.dns_ = dns_view(*document, query_name, query_type);
                if (facts.dns_ && !facts.dns_->rcode_number_.empty() && facts.dns_->rcode_number_ != "0" &&
                    facts.provider_code_.empty()) {
                    facts.provider_code_ = text(facts.dns_->rcode_, identifier_bytes);
                }
            }
        } catch (...) {
            failed_ = true;
        }
        return facts;
    }

    // A DNS query whose answer could not be read (transport failure or a
    // reply that is not DNS JSON): the query itself, no rcode, no answers.
    [[nodiscard]] dns_observation unanswered_query(std::string_view name, std::string_view type) noexcept {
        dns_observation result;
        result.name_ = text(name, value_bytes);
        result.type_ = text(type, identifier_bytes);
        return result;
    }

    [[nodiscard]] redacted_text marker() noexcept {
        try {
            return redacted_text(std::string(redacted_marker));
        } catch (...) {
            failed_ = true;
            return {};
        }
    }

    // Values replaced by the marker so far.
    [[nodiscard]] std::size_t redactions() const noexcept {
        return redactions_;
    }

    // An internal failure; the text concerned was dropped, never kept.
    [[nodiscard]] bool failed() const noexcept {
        return failed_;
    }

    [[nodiscard]] static bool contains_private_key(std::string_view value) noexcept {
        return value.find("PRIVATE KEY-----") != std::string_view::npos;
    }

private:
    static constexpr std::size_t minimum_secret_bytes{8};
    static constexpr std::size_t maximum_secrets{512};
    // Room for credentials beyond the observed values (each credential takes
    // up to seven encoded variants).
    static constexpr std::size_t maximum_credential_variants{1024};
    static constexpr int maximum_json_depth{16};

    [[nodiscard]] bool known(std::string_view value) const noexcept {
        return std::any_of(secrets_.begin(), secrets_.end(), [value](const auto& secret) { return secret.view() == value; });
    }

    // A value seen in traffic outside the allow-lists (masked URL parts,
    // nonces, signatures, response cookies): remembered while fewer than
    // maximum_secrets are known. The patterns still cover what is not.
    void remember(std::string_view value) noexcept {
        try {
            if (value.size() < minimum_secret_bytes || secrets_.size() >= maximum_secrets) {
                return;
            }
            add_secret(value);
        } catch (...) {
            failed_ = true;
        }
    }

    void add_secret(std::string_view value) {
        std::string json;
        text_detail::append_json_string(json, value);
        const std::array variants{std::string(value), text_detail::percent_encode(value, true, false),
            text_detail::percent_encode(value, false, false), text_detail::percent_encode(value, true, true),
            text_detail::base64_encode(value, false), text_detail::base64_encode(value, true),
            json.substr(1, json.size() - 2)};
        for (const auto& variant : variants) {
            if (variant.size() < minimum_secret_bytes || known(variant)) {
                continue;
            }
            secrets_.emplace_back(std::string(variant));
        }
        std::sort(secrets_.begin(), secrets_.end(),
            [](const auto& left, const auto& right) { return left.view().size() > right.view().size(); });
    }

    [[nodiscard]] redacted_body omit(redacted_body result, std::string_view reason) noexcept {
        result.format_ = "omitted";
        result.omitted_reason_ = reason;
        result.excerpt_ = redacted_text{};
        result.truncated_ = false;
        return result;
    }

    [[nodiscard]] static bool valid_text(std::string_view value) noexcept {
        for (std::size_t index = 0; index < value.size();) {
            const auto length = service::utils::utf8_sequence_length(value, index);
            if (length == 0) {
                return false;
            }
            const auto lead = static_cast<unsigned char>(value[index]);
            if (length == 1 && lead < 0x20 && lead != '\n' && lead != '\r' && lead != '\t') {
                return false;
            }
            index += length;
        }
        return true;
    }

    [[nodiscard]] static std::string_view unquoted(const ruvia::json_value& value) noexcept {
        auto view = value.view();
        if (view.size() >= 2 && view.front() == '"' && view.back() == '"') {
            view = view.substr(1, view.size() - 2);
        }
        return view;
    }

    // Exact secret replacement followed by the pattern scans.
    [[nodiscard]] std::string scrub(std::string value, bool& private_key) {
        for (const auto& secret : secrets_) {
            replace_all(value, secret.view());
        }
        value = scrub_pem(value, private_key);
        value = scrub_bearer(value);
        value = scrub_jwt(value);
        value = scrub_signed_strings(value);
        value = scrub_parameters(value);
        return value;
    }

    void replace_all(std::string& value, std::string_view secret) {
        if (secret.empty() || value.size() < secret.size()) {
            return;
        }
        auto found = value.find(secret);
        if (found == std::string::npos) {
            return;
        }
        std::string result;
        result.reserve(value.size());
        std::size_t position = 0;
        while (found != std::string::npos) {
            result.append(value, position, found - position);
            result.append(redacted_marker);
            ++redactions_;
            position = found + secret.size();
            found = value.find(secret, position);
        }
        result.append(value, position, std::string::npos);
        OPENSSL_cleanse(value.data(), value.size());
        value = std::move(result);
    }

    // PEM blocks: private keys become the marker, other blocks their size.
    [[nodiscard]] std::string scrub_pem(const std::string& value, bool& private_key) {
        constexpr std::string_view begin = "-----BEGIN ";
        auto found = value.find(begin);
        if (found == std::string::npos) {
            return value;
        }
        std::string result;
        std::size_t position = 0;
        while (found != std::string::npos) {
            result.append(value, position, found - position);
            const auto label_start = found + begin.size();
            const auto label_end = value.find("-----", label_start);
            const auto label = label_end == std::string::npos || label_end - label_start > 64
                                   ? std::string_view{}
                                   : std::string_view(value).substr(label_start, label_end - label_start);
            const auto end_marker = "-----END " + std::string(label) + "-----";
            const auto end = label.empty() ? std::string::npos : value.find(end_marker, label_end);
            const auto block_end = end == std::string::npos ? value.size() : end + end_marker.size();
            if (label.empty() || label.find("PRIVATE KEY") != std::string_view::npos) {
                private_key = true;
                result.append(redacted_marker);
            } else {
                result.append("[PEM ").append(text_detail::sanitized(label, false)).append(" ");
                result.append(std::to_string(block_end - found)).append(" bytes]");
            }
            ++redactions_;
            position = block_end;
            found = value.find(begin, position);
        }
        result.append(value, position, std::string::npos);
        return result;
    }

    [[nodiscard]] std::string scrub_bearer(const std::string& value) {
        std::string result;
        result.reserve(value.size());
        std::size_t index = 0;
        while (index < value.size()) {
            if (text_detail::matches_ignore_case_at(value, index, "bearer") &&
                (index == 0 || !text_detail::is_alnum(value[index - 1]))) {
                auto token = index + 6;
                while (token < value.size() && (value[token] == ' ' || value[token] == '\t')) {
                    ++token;
                }
                auto end = token;
                while (end < value.size() && (text_detail::is_base64_url(value[end]) || value[end] == '.' ||
                                                 value[end] == '~' || value[end] == '+' || value[end] == '/' ||
                                                 value[end] == '=')) {
                    ++end;
                }
                if (token > index + 6 && end > token) {
                    result.append(value, index, token - index);
                    result.append(redacted_marker);
                    ++redactions_;
                    index = end;
                    continue;
                }
            }
            result.push_back(value[index]);
            ++index;
        }
        return result;
    }

    [[nodiscard]] std::string scrub_jwt(const std::string& value) {
        auto found = value.find("eyJ");
        if (found == std::string::npos) {
            return value;
        }
        std::string result;
        std::size_t position = 0;
        while (found != std::string::npos) {
            const auto segment_end = [&](std::size_t start) {
                while (start < value.size() && text_detail::is_base64_url(value[start])) {
                    ++start;
                }
                return start;
            };
            const bool boundary = found == 0 || !text_detail::is_base64_url(value[found - 1]);
            const auto first = segment_end(found);
            std::size_t end = std::string::npos;
            if (boundary && first - found >= 10 && first < value.size() && value[first] == '.') {
                const auto second = segment_end(first + 1);
                if (second > first + 1 && second < value.size() && value[second] == '.') {
                    end = segment_end(second + 1);
                }
            }
            if (end == std::string::npos) {
                result.append(value, position, found + 3 - position);
                position = found + 3;
            } else {
                result.append(value, position, found - position);
                result.append(redacted_marker);
                ++redactions_;
                position = end;
            }
            found = value.find("eyJ", position);
        }
        result.append(value, position, std::string::npos);
        return result;
    }

    // The material a provider echoes after a signed-string phrase (see
    // text_detail::signed_string_phrases) is dropped; the phrase itself and
    // the surrounding message stay readable. The echo ends at whitespace, a
    // quote, a backslash or `<`; inside an XML element (`>` right after the
    // phrase) only at `<`.
    [[nodiscard]] std::string scrub_signed_strings(const std::string& value) {
        std::string result;
        result.reserve(value.size());
        std::size_t index = 0;
        while (index < value.size()) {
            std::size_t phrase = 0;
            for (const auto candidate : text_detail::signed_string_phrases) {
                if (text_detail::matches_ignore_case_at(value, index, candidate)) {
                    phrase = candidate.size();
                    break;
                }
            }
            if (phrase == 0) {
                result.push_back(value[index]);
                ++index;
                continue;
            }
            auto cursor = index + phrase;
            if (text_detail::matches_ignore_case_at(value, cursor, " is")) {
                cursor += 3;
            }
            bool element = false;
            while (cursor < value.size() &&
                   (value[cursor] == ' ' || value[cursor] == ':' || value[cursor] == '=' || value[cursor] == '>')) {
                element = element || value[cursor] == '>';
                ++cursor;
            }
            auto end = cursor;
            while (end < value.size()) {
                const auto ch = value[end];
                if (ch == '<' || (!element && (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '"' ||
                                                  ch == '\'' || ch == '\\'))) {
                    break;
                }
                ++end;
            }
            result.append(value, index, cursor - index);
            if (end > cursor) {
                result.append(redacted_marker);
                ++redactions_;
            }
            index = end;
        }
        return result;
    }

    // `AccessKeyId=…`, `Signature%3D…`, `"token": "…"` and similar pairs in
    // free text, e.g. a provider echoing the string it signed.
    [[nodiscard]] std::string scrub_parameters(const std::string& value) {
        std::string result;
        result.reserve(value.size());
        std::size_t index = 0;
        while (index < value.size()) {
            std::size_t value_start = std::string::npos;
            if (index == 0 || (!text_detail::is_alnum(value[index - 1]) && value[index - 1] != '_')) {
                for (const auto name : text_detail::sensitive_parameter_names) {
                    if (!text_detail::matches_ignore_case_at(value, index, name)) {
                        continue;
                    }
                    auto cursor = index + name.size();
                    if (cursor < value.size() && value[cursor] == '"') {
                        ++cursor;
                    }
                    while (cursor < value.size() && value[cursor] == ' ') {
                        ++cursor;
                    }
                    if (cursor < value.size() && (value[cursor] == '=' || value[cursor] == ':')) {
                        ++cursor;
                    } else if (text_detail::matches_ignore_case_at(value, cursor, "%3D")) {
                        cursor += 3;
                    } else {
                        continue;
                    }
                    while (cursor < value.size() && value[cursor] == ' ') {
                        ++cursor;
                    }
                    if (cursor < value.size() && value[cursor] == '"') {
                        ++cursor;
                    }
                    value_start = cursor;
                    break;
                }
            }
            if (value_start == std::string::npos) {
                result.push_back(value[index]);
                ++index;
                continue;
            }
            auto end = value_start;
            while (end < value.size()) {
                const auto ch = value[end];
                if (ch == '&' || ch == '"' || ch == ',' || ch == ' ' || ch == '\n' || ch == '\t' || ch == '}' ||
                    ch == ';' || ch == '\'' || text_detail::matches_ignore_case_at(value, end, "%26")) {
                    break;
                }
                ++end;
            }
            result.append(value, index, value_start - index);
            const auto secret = std::string_view(value).substr(value_start, end - value_start);
            if (secret.empty() || secret.starts_with(redacted_marker)) {
                result.append(secret);
            } else {
                result.append(redacted_marker);
                ++redactions_;
            }
            index = end;
        }
        return result;
    }

    // Allow-listed, pretty-printed JSON; nullopt when the body is not JSON.
    [[nodiscard]] std::optional<std::string> json_view(std::string_view body, std::span<const std::string_view> allowed,
        std::span<const std::string_view> sized) {
        std::pmr::monotonic_buffer_resource pool;
        const auto document = ruvia::json_value::parse(body, {.resource_ = &pool});
        if (!document) {
            return std::nullopt;
        }
        std::string output;
        std::string path;
        render_json(*document, path, allowed, sized, {}, false, 0, output);
        return output;
    }

    void render_json(const ruvia::json_value& value, std::string& path, std::span<const std::string_view> allowed,
        std::span<const std::string_view> sized, std::span<const std::string_view> dropped, bool subtree, int depth,
        std::string& output) {
        const auto indent = [&](int level) { output.append(static_cast<std::size_t>(level) * 2, ' '); };
        const auto kind = value.kind();
        if (kind == ruvia::json_value::kind_type::object || kind == ruvia::json_value::kind_type::array) {
            const bool descend = subtree || path.empty() || container_allowed(allowed, path);
            if (!descend || depth >= maximum_json_depth) {
                text_detail::append_json_string(output, redacted_marker);
                ++redactions_;
                return;
            }
            const bool whole = subtree || (!path.empty() && text_detail::contains(allowed, path));
            if (kind == ruvia::json_value::kind_type::object) {
                std::size_t written = 0;
                output.push_back('{');
                (void)value.for_each_field([&](std::string_view key, const ruvia::json_value& field) {
                    if (path.empty() && text_detail::contains(dropped, key)) {
                        return true;
                    }
                    output.append(written++ == 0 ? "\n" : ",\n");
                    indent(depth + 1);
                    text_detail::append_json_string(output, text_detail::sanitized(key, false));
                    output.append(": ");
                    const auto previous = path.size();
                    if (!path.empty()) {
                        path.push_back('.');
                    }
                    path.append(key);
                    render_json(field, path, allowed, sized, {}, whole, depth + 1, output);
                    path.resize(previous);
                    return true;
                });
                if (written > 0) {
                    output.push_back('\n');
                    indent(depth);
                }
                output.push_back('}');
                return;
            }
            std::size_t count = 0;
            output.push_back('[');
            const auto previous = path.size();
            path.append("[]");
            (void)value.for_each_element([&](const ruvia::json_value& element) {
                if (count++ >= max_json_items) {
                    return true;
                }
                output.append(count == 1 ? "\n" : ",\n");
                indent(depth + 1);
                render_json(element, path, allowed, sized, {}, whole, depth + 1, output);
                return true;
            });
            path.resize(previous);
            if (count > max_json_items) {
                items_omitted_ = true;
                output.append(",\n");
                indent(depth + 1);
                text_detail::append_json_string(output, "… " + std::to_string(count - max_json_items) + " more");
            }
            if (count > 0) {
                output.push_back('\n');
                indent(depth);
            }
            output.push_back(']');
            return;
        }
        if (kind == ruvia::json_value::kind_type::null) {
            output.append("null");
            return;
        }
        if (!subtree && !text_detail::contains(allowed, path)) {
            if (kind == ruvia::json_value::kind_type::string && text_detail::contains(sized, path)) {
                const auto decoded = value.get<ruvia::string>();
                const auto name = path.substr(path.rfind('.') == std::string::npos ? 0 : path.rfind('.') + 1);
                std::string upper(name);
                for (auto& ch : upper) {
                    ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                }
                text_detail::append_json_string(output, "[" + upper + " " +
                                                            std::to_string(decoded ? decoded->view().size() : 0) +
                                                            " bytes]");
                return;
            }
            text_detail::append_json_string(output, redacted_marker);
            ++redactions_;
            return;
        }
        if (kind == ruvia::json_value::kind_type::string) {
            const auto decoded = value.get<ruvia::string>();
            text_detail::append_json_string(output, decoded ? text(decoded->view(), value_bytes).view()
                                                            : std::string_view{redacted_marker});
            return;
        }
        output.append(text_detail::sanitized(value.view(), false));
    }

    [[nodiscard]] static bool container_allowed(std::span<const std::string_view> allowed, std::string_view path) {
        return std::any_of(allowed.begin(), allowed.end(), [path](std::string_view candidate) {
            return candidate == path ||
                   (candidate.size() > path.size() && candidate.starts_with(path) &&
                       (candidate[path.size()] == '.' || candidate[path.size()] == '['));
        });
    }

    // ACME JWS: the allow-listed protected header (alg, url; kid and jwk
    // masked, nonce dropped) and the decoded payload. The signature is never kept.
    [[nodiscard]] std::optional<std::string> jws_view(const outbound_policy& policy, std::string_view body) {
        std::pmr::monotonic_buffer_resource pool;
        const auto document = ruvia::json_value::parse(body, {.resource_ = &pool});
        if (!document || !document->is_object()) {
            return std::nullopt;
        }
        static constexpr std::array protected_fields{std::string_view{"alg"}, std::string_view{"url"}};
        static constexpr std::array dropped_fields{std::string_view{"nonce"}};
        std::string output = "{\n  \"protected\": ";
        const auto protected_value = document->get<ruvia::string>("protected");
        const auto protected_json = protected_value ? text_detail::base64_url_decode(protected_value->view()) : std::nullopt;
        std::optional<ruvia::json_value> header;
        if (protected_json) {
            header = ruvia::json_value::parse(std::string_view(*protected_json), {.resource_ = &pool});
        }
        if (header && header->is_object()) {
            std::string path;
            render_json(*header, path, protected_fields, {}, dropped_fields, false, 1, output);
        } else {
            text_detail::append_json_string(output, redacted_marker);
            ++redactions_;
        }
        output.append(",\n  \"payload\": ");
        const auto payload_value = document->get<ruvia::string>("payload");
        const auto payload = payload_value ? text_detail::base64_url_decode(payload_value->view()) : std::nullopt;
        if (payload && payload->empty()) {
            text_detail::append_json_string(output, "(POST-as-GET)");
        } else {
            std::optional<ruvia::json_value> parsed;
            if (payload) {
                parsed = ruvia::json_value::parse(std::string_view(*payload), {.resource_ = &pool});
            }
            if (parsed) {
                std::string path;
                render_json(*parsed, path, policy.request_fields_, policy.request_sized_fields_, {}, false, 1, output);
            } else {
                text_detail::append_json_string(output, redacted_marker);
                ++redactions_;
            }
        }
        output.append("\n}");
        if (const auto signature = document->get<ruvia::string>("signature"); signature) {
            remember(signature->view());
        }
        return output;
    }

    [[nodiscard]] std::string form_view(const outbound_policy& policy, std::string_view body) {
        std::string output;
        std::size_t start = 0;
        while (start < body.size()) {
            const auto end = std::min(body.find('&', start), body.size());
            const auto part = body.substr(start, end - start);
            start = end + 1;
            if (part.empty()) {
                continue;
            }
            const auto equals = part.find('=');
            const auto name = text_detail::percent_decode(part.substr(0, equals));
            const auto value = equals == std::string_view::npos ? std::string{}
                                                                : text_detail::percent_decode(part.substr(equals + 1));
            if (!output.empty()) {
                output.push_back('&');
            }
            output.append(name).push_back('=');
            if (text_detail::contains(policy.request_fields_, name)) {
                output.append(value);
            } else {
                remember(value);
                output.append(redacted_marker);
                ++redactions_;
            }
        }
        return output;
    }

    // Subject, issuer, serial number and expiry of each certificate; the
    // encoded certificates themselves are not kept.
    [[nodiscard]] std::optional<std::string> certificate_chain_view(std::string_view body) {
        const std::unique_ptr<BIO, decltype(&BIO_free)> input(
            BIO_new_mem_buf(body.data(), static_cast<int>(body.size())), &BIO_free);
        if (!input) {
            return std::nullopt;
        }
        std::string output = "[";
        std::size_t count = 0;
        while (true) {
            const std::unique_ptr<X509, decltype(&X509_free)> certificate(
                PEM_read_bio_X509(input.get(), nullptr, nullptr, nullptr), &X509_free);
            if (!certificate) {
                break;
            }
            if (count++ >= max_json_items) {
                continue;
            }
            output.append(count == 1 ? "\n" : ",\n");
            output.append("  {\n    \"subject\": ");
            text_detail::append_json_string(output, text(name_text(X509_get_subject_name(certificate.get())), value_bytes).view());
            output.append(",\n    \"issuer\": ");
            text_detail::append_json_string(output, text(name_text(X509_get_issuer_name(certificate.get())), value_bytes).view());
            output.append(",\n    \"serial_number\": ");
            text_detail::append_json_string(output, serial_text(certificate.get()));
            output.append(",\n    \"not_after\": ");
            text_detail::append_json_string(output, time_text(X509_get0_notAfter(certificate.get())));
            output.append("\n  }");
        }
        ERR_clear_error();
        if (count == 0) {
            return std::nullopt;
        }
        output.append(count > 0 ? "\n]" : "]");
        return output;
    }

    [[nodiscard]] static std::string name_text(const X509_NAME* name) {
        const std::unique_ptr<BIO, decltype(&BIO_free)> output(BIO_new(BIO_s_mem()), &BIO_free);
        if (!output || name == nullptr || X509_NAME_print_ex(output.get(), name, 0, XN_FLAG_RFC2253) < 0) {
            return {};
        }
        char* data = nullptr;
        const auto size = BIO_get_mem_data(output.get(), &data);
        return size > 0 && data != nullptr ? std::string(data, static_cast<std::size_t>(size)) : std::string{};
    }

    [[nodiscard]] static std::string serial_text(X509* certificate) {
        const std::unique_ptr<BIGNUM, decltype(&BN_free)> number(
            ASN1_INTEGER_to_BN(X509_get_serialNumber(certificate), nullptr), &BN_free);
        if (!number) {
            return {};
        }
        char* hex = BN_bn2hex(number.get());
        if (hex == nullptr) {
            return {};
        }
        std::string result(hex);
        OPENSSL_free(hex);
        return result;
    }

    [[nodiscard]] static std::string time_text(const ASN1_TIME* value) {
        std::tm parts{};
        if (value == nullptr || ASN1_TIME_to_tm(value, &parts) != 1) {
            return {};
        }
        std::array<char, 32> buffer{};
        const auto size = std::strftime(buffer.data(), buffer.size(), "%Y-%m-%dT%H:%M:%SZ", &parts);
        return std::string(buffer.data(), size);
    }

    [[nodiscard]] dns_observation dns_view(const ruvia::json_value& document, std::string_view query_name,
        std::string_view query_type) {
        dns_observation result;
        result.name_ = text(query_name, value_bytes);
        result.type_ = text(query_type, identifier_bytes);
        if (const auto status = document.get<ruvia::int64>("Status"); status) {
            const auto value = static_cast<std::int64_t>(*status);
            result.rcode_number_ = std::to_string(value);
            const auto name = text_detail::dns_rcode_name(value);
            result.rcode_ = name.empty() ? "RCODE" + result.rcode_number_ : std::string(name);
        }
        if (const auto answers = document.get<ruvia::json_value>("Answer"); answers && answers->is_array()) {
            (void)answers->for_each_element([&](const ruvia::json_value& answer) {
                if (result.answers_.size() >= max_dns_answers) {
                    result.truncated_ = true;
                    return true;
                }
                dns_answer item;
                if (const auto name = answer.get<ruvia::string>("name"); name) {
                    item.name_ = text(name->view(), value_bytes);
                }
                if (const auto type = answer.get<ruvia::int64>("type"); type) {
                    const auto number = static_cast<std::int64_t>(*type);
                    const auto name = text_detail::dns_type_name(number);
                    item.type_ = text(name.empty() ? "TYPE" + std::to_string(number) : std::string(name), identifier_bytes);
                }
                if (const auto ttl = answer.get<ruvia::int64>("TTL"); ttl) {
                    item.ttl_ = static_cast<std::int64_t>(*ttl);
                }
                if (const auto data = answer.get<ruvia::string>("data"); data) {
                    item.data_ = text(data->view(), value_bytes);
                }
                result.answers_.push_back(std::move(item));
                return true;
            });
        }
        return result;
    }

    std::vector<service::utils::sensitive_string> secrets_;
    std::size_t redactions_{};
    bool items_omitted_{};
    bool failed_{};
    bool secrets_incomplete_{};
};

// Replaces error text that could not be shown without risking a credential.
inline constexpr std::string_view redacted_error_fallback{"错误详情可能包含凭据，已隐藏"};

// Error text with no attempt in scope (a failure that escaped every
// attempt): only the credential patterns are known, so only they are
// replaced.
[[nodiscard]] inline std::string redact_patterns(std::string_view value) noexcept {
    redactor patterns;
    return patterns.external_text(value, redacted_error_fallback);
}

}  // namespace service::attempt_trace
