#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

// How the recorder reduces one outbound operation of a background attempt.
// Each provider declares the policies of its own operations next to its
// client; anything a policy does not list is stored as "[REDACTED]": query
// values, header values, JSON fields (path syntax `a.b`, `list[]` for array
// elements) and form keys.
namespace service::attempt_trace {

// How a body of an operation is reduced. `jws` decodes an ACME JWS into its
// allow-listed protected header and payload; `pem_chain` keeps only the
// identity of each certificate.
enum class body_rule : std::uint8_t {
    none,
    json,
    jws,
    form,
    pem_chain,
};

// Where a provider's reply names its request id and its error code.
struct reply_identity final {
    // Response headers carrying the request id, first present wins; their
    // values are kept in the trace.
    std::span<const std::string_view> request_id_headers_{};
    // Top-level JSON string field carrying the request id when no header does.
    std::string_view request_id_field_{};
    // JSON fields carrying the provider error code (path syntax as above, an
    // array path reads its first element), first present wins; a string or a
    // number.
    std::span<const std::string_view> error_code_fields_{};
    // Error code fields are read only from a failed reply (status >= 400) or
    // a problem document (application/problem+json).
    bool error_code_on_failure_only_{};
};

// Every list defaults to empty: nothing of that part is kept.
struct outbound_policy final {
    std::string_view code_;
    std::span<const std::string_view> query_{};
    // A path segment following one of these segments is masked (account ids).
    std::span<const std::string_view> masked_path_after_{};
    body_rule request_body_{body_rule::none};
    std::span<const std::string_view> request_fields_{};
    // Request payload fields shown only as their size, e.g. the CSR.
    std::span<const std::string_view> request_sized_fields_{};
    body_rule response_body_{body_rule::none};
    std::span<const std::string_view> response_fields_{};
    reply_identity identity_{};
    // DNS over HTTPS: the call is a DNS query carried by HTTP.
    bool dns_query_{};
};

// Header values kept verbatim; every other header keeps its name only. A
// response also keeps the request id headers of its policy.
inline constexpr std::array request_header_allow_list{std::string_view{"accept"}, std::string_view{"content-type"},
    std::string_view{"content-length"}, std::string_view{"user-agent"}};
inline constexpr std::array response_header_allow_list{std::string_view{"content-type"},
    std::string_view{"content-length"}, std::string_view{"date"}, std::string_view{"retry-after"},
    std::string_view{"cache-control"}, std::string_view{"x-request-id"}, std::string_view{"link"},
    std::string_view{"location"}};
// Response headers whose value is a URL; they pass through the URL redaction.
inline constexpr std::array url_response_headers{std::string_view{"link"}, std::string_view{"location"}};

// What a call site tells the recorder: the configured origin (scheme and host
// come from the outbound configuration, never from the request), the policy
// of the operation and an optional subject.
struct outbound_call final {
    std::string_view origin_alias_;
    const outbound_policy* policy_{};
    std::string_view subject_{};
};

}  // namespace service::attempt_trace
