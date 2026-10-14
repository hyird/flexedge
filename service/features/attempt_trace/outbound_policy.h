#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

// Allow-lists of every outbound operation a background attempt performs.
// Anything not listed is stored as "[REDACTED]": query values, header values,
// JSON fields (path syntax `a.b`, `list[]` for array elements) and form keys.
namespace service::attempt_trace {

enum class outbound_operation : std::uint8_t {
    cloudflare_verify_token,
    cloudflare_list_zones,
    cloudflare_find_zone,
    cloudflare_list_records,
    cloudflare_find_record,
    cloudflare_create_record,
    cloudflare_update_record,
    cloudflare_delete_record,
    aliyun_describe_domains,
    aliyun_describe_domain_records,
    aliyun_describe_support_lines,
    aliyun_add_domain_record,
    aliyun_update_domain_record,
    aliyun_delete_domain_record,
    acme_directory,
    acme_new_nonce,
    acme_new_account,
    acme_new_order,
    acme_authorization,
    acme_challenge,
    acme_finalize,
    acme_order,
    acme_certificate,
    zerossl_eab_by_email,
    zerossl_eab_by_access_key,
    doh_resolve,
};

// How a body of this operation is reduced. `jws` decodes an ACME JWS into
// its allow-listed protected header and payload; `pem_chain` keeps only the
// identity of each certificate.
enum class body_rule : std::uint8_t {
    none,
    json,
    jws,
    form,
    pem_chain,
};

struct outbound_policy final {
    outbound_operation operation_;
    std::string_view code_;
    std::span<const std::string_view> query_;
    // A path segment following one of these segments is masked (account ids).
    std::span<const std::string_view> masked_path_after_;
    body_rule request_body_;
    std::span<const std::string_view> request_fields_;
    // Request payload fields shown only as their size, e.g. the CSR.
    std::span<const std::string_view> request_sized_fields_;
    body_rule response_body_;
    std::span<const std::string_view> response_fields_;
    // DNS over HTTPS: the call is a DNS query carried by HTTP.
    bool dns_query_;
};

// Header values kept verbatim; every other header keeps its name only.
inline constexpr std::array request_header_allow_list{std::string_view{"accept"}, std::string_view{"content-type"},
    std::string_view{"content-length"}, std::string_view{"user-agent"}};
inline constexpr std::array response_header_allow_list{std::string_view{"content-type"},
    std::string_view{"content-length"}, std::string_view{"date"}, std::string_view{"retry-after"},
    std::string_view{"cache-control"}, std::string_view{"cf-ray"}, std::string_view{"x-acs-request-id"},
    std::string_view{"x-request-id"}, std::string_view{"link"}, std::string_view{"location"}};
// Response headers whose value is a URL; they pass through the URL redaction.
inline constexpr std::array url_response_headers{std::string_view{"link"}, std::string_view{"location"}};

namespace policy_lists {

using namespace std::string_view_literals;

inline constexpr std::array<std::string_view, 0> none{};
inline constexpr std::array accounts_path{"accounts"sv};

inline constexpr std::array cloudflare_zone_query{"name"sv, "status"sv, "order"sv, "direction"sv, "page"sv,
    "per_page"sv};
inline constexpr std::array cloudflare_record_query{"order"sv, "direction"sv, "page"sv, "per_page"sv, "match"sv,
    "type"sv, "name.exact"sv, "content.exact"sv, "search"sv};
inline constexpr std::array cloudflare_token_response{"success"sv, "errors[].code"sv, "errors[].message"sv,
    "messages[].code"sv, "messages[].message"sv, "result.status"sv, "result.expires_on"sv, "result.not_before"sv};
inline constexpr std::array cloudflare_zone_response{"success"sv, "errors[].code"sv, "errors[].message"sv,
    "messages[].code"sv, "messages[].message"sv, "result_info.page"sv, "result_info.per_page"sv,
    "result_info.count"sv, "result_info.total_count"sv, "result_info.total_pages"sv, "result[].id"sv,
    "result[].name"sv, "result[].status"sv, "result[].paused"sv, "result[].type"sv, "result[].name_servers[]"sv};
inline constexpr std::array cloudflare_record_list_response{"success"sv, "errors[].code"sv, "errors[].message"sv,
    "messages[].code"sv, "messages[].message"sv, "result_info.page"sv, "result_info.per_page"sv,
    "result_info.count"sv, "result_info.total_count"sv, "result_info.total_pages"sv, "result[].id"sv,
    "result[].type"sv, "result[].name"sv, "result[].content"sv, "result[].ttl"sv, "result[].proxied"sv,
    "result[].priority"sv, "result[].zone_name"sv, "result[].created_on"sv, "result[].modified_on"sv};
inline constexpr std::array cloudflare_record_response{"success"sv, "errors[].code"sv, "errors[].message"sv,
    "messages[].code"sv, "messages[].message"sv, "result.id"sv, "result.type"sv, "result.name"sv,
    "result.content"sv, "result.ttl"sv, "result.proxied"sv, "result.priority"sv, "result.modified_on"sv};
inline constexpr std::array cloudflare_record_request{"type"sv, "name"sv, "content"sv, "ttl"sv, "priority"sv,
    "proxied"sv};

// AccessKeyId, Signature and SignatureNonce are deliberately absent.
inline constexpr std::array aliyun_query{"Action"sv, "DomainName"sv, "RR"sv, "Type"sv, "Value"sv, "TTL"sv,
    "Line"sv, "Priority"sv, "RecordId"sv, "PageNumber"sv, "PageSize"sv, "Format"sv, "Version"sv,
    "SignatureMethod"sv, "SignatureVersion"sv, "Timestamp"sv};
inline constexpr std::array aliyun_response{"RequestId"sv, "Code"sv, "Message"sv, "HostId"sv, "Recommend"sv,
    "TotalCount"sv, "PageNumber"sv, "PageSize"sv, "RecordId"sv, "DomainRecords.Record[].RecordId"sv,
    "DomainRecords.Record[].RR"sv, "DomainRecords.Record[].Type"sv, "DomainRecords.Record[].Value"sv,
    "DomainRecords.Record[].TTL"sv, "DomainRecords.Record[].Line"sv, "DomainRecords.Record[].Status"sv,
    "DomainRecords.Record[].Locked"sv, "DomainRecords.Record[].Priority"sv, "DomainRecords.Record[].Weight"sv,
    "DomainRecords.Record[].DomainName"sv, "Domains.Domain[].DomainId"sv, "Domains.Domain[].DomainName"sv,
    "Domains.Domain[].PunyCode"sv, "Domains.Domain[].RecordCount"sv, "Domains.Domain[].DnsServers.DnsServer[]"sv,
    "RecordLines.RecordLine[].LineCode"sv, "RecordLines.RecordLine[].LineName"sv,
    "RecordLines.RecordLine[].FatherCode"sv, "RecordLines.RecordLine[].LineDisplayName"sv};

inline constexpr std::array acme_directory_response{"newNonce"sv, "newAccount"sv, "newOrder"sv, "revokeCert"sv,
    "keyChange"sv, "renewalInfo"sv, "meta.termsOfService"sv, "meta.website"sv, "meta.caaIdentities[]"sv,
    "meta.externalAccountRequired"sv, "type"sv, "detail"sv, "status"sv};
inline constexpr std::array acme_account_request{"termsOfServiceAgreed"sv, "onlyReturnExisting"sv};
inline constexpr std::array acme_account_response{"status"sv, "orders"sv, "createdAt"sv, "type"sv, "detail"sv,
    "subproblems[].type"sv, "subproblems[].detail"sv, "subproblems[].identifier.type"sv,
    "subproblems[].identifier.value"sv};
inline constexpr std::array acme_order_request{"identifiers[].type"sv, "identifiers[].value"sv};
inline constexpr std::array acme_order_response{"status"sv, "expires"sv, "notBefore"sv, "notAfter"sv,
    "identifiers[].type"sv, "identifiers[].value"sv, "authorizations[]"sv, "finalize"sv, "certificate"sv,
    "error.type"sv, "error.detail"sv, "error.status"sv, "error.subproblems[].type"sv,
    "error.subproblems[].detail"sv, "error.subproblems[].identifier.type"sv,
    "error.subproblems[].identifier.value"sv, "type"sv, "detail"sv, "subproblems[].type"sv,
    "subproblems[].detail"sv, "subproblems[].identifier.type"sv, "subproblems[].identifier.value"sv};
inline constexpr std::array acme_authorization_response{"status"sv, "expires"sv, "wildcard"sv,
    "identifier.type"sv, "identifier.value"sv, "challenges[].type"sv, "challenges[].status"sv,
    "challenges[].url"sv, "challenges[].validated"sv, "challenges[].error.type"sv, "challenges[].error.detail"sv,
    "challenges[].error.status"sv, "type"sv, "detail"sv, "subproblems[].type"sv, "subproblems[].detail"sv};
inline constexpr std::array acme_challenge_response{"type"sv, "status"sv, "url"sv, "validated"sv, "error.type"sv,
    "error.detail"sv, "error.status"sv, "detail"sv, "subproblems[].type"sv, "subproblems[].detail"sv};
inline constexpr std::array acme_problem_response{"type"sv, "detail"sv, "status"sv, "subproblems[].type"sv,
    "subproblems[].detail"sv, "subproblems[].identifier.type"sv, "subproblems[].identifier.value"sv};
inline constexpr std::array acme_csr_field{"csr"sv};

// eab_kid and eab_hmac_key are deliberately absent.
inline constexpr std::array zerossl_eab_response{"success"sv, "error.code"sv, "error.type"sv, "error.info"sv};

inline constexpr std::array doh_query{"name"sv, "type"sv, "cd"sv, "do"sv};
inline constexpr std::array doh_response{"Status"sv, "TC"sv, "RD"sv, "RA"sv, "AD"sv, "CD"sv, "Question[].name"sv,
    "Question[].type"sv, "Answer[].name"sv, "Answer[].type"sv, "Answer[].TTL"sv, "Answer[].data"sv,
    "Authority[].name"sv, "Authority[].type"sv, "Authority[].TTL"sv, "Authority[].data"sv, "Comment"sv};

}  // namespace policy_lists

inline constexpr std::array outbound_policies{
    outbound_policy{outbound_operation::cloudflare_verify_token, "cloudflare.verify_token", policy_lists::none,
        policy_lists::accounts_path, body_rule::none, policy_lists::none, policy_lists::none, body_rule::json,
        policy_lists::cloudflare_token_response, false},
    outbound_policy{outbound_operation::cloudflare_list_zones, "cloudflare.list_zones",
        policy_lists::cloudflare_zone_query, policy_lists::none, body_rule::none, policy_lists::none,
        policy_lists::none, body_rule::json, policy_lists::cloudflare_zone_response, false},
    outbound_policy{outbound_operation::cloudflare_find_zone, "cloudflare.find_zone",
        policy_lists::cloudflare_zone_query, policy_lists::none, body_rule::none, policy_lists::none,
        policy_lists::none, body_rule::json, policy_lists::cloudflare_zone_response, false},
    outbound_policy{outbound_operation::cloudflare_list_records, "cloudflare.list_records",
        policy_lists::cloudflare_record_query, policy_lists::none, body_rule::none, policy_lists::none,
        policy_lists::none, body_rule::json, policy_lists::cloudflare_record_list_response, false},
    outbound_policy{outbound_operation::cloudflare_find_record, "cloudflare.find_record",
        policy_lists::cloudflare_record_query, policy_lists::none, body_rule::none, policy_lists::none,
        policy_lists::none, body_rule::json, policy_lists::cloudflare_record_list_response, false},
    outbound_policy{outbound_operation::cloudflare_create_record, "cloudflare.create_record", policy_lists::none,
        policy_lists::none, body_rule::json, policy_lists::cloudflare_record_request, policy_lists::none,
        body_rule::json, policy_lists::cloudflare_record_response, false},
    outbound_policy{outbound_operation::cloudflare_update_record, "cloudflare.update_record", policy_lists::none,
        policy_lists::none, body_rule::json, policy_lists::cloudflare_record_request, policy_lists::none,
        body_rule::json, policy_lists::cloudflare_record_response, false},
    outbound_policy{outbound_operation::cloudflare_delete_record, "cloudflare.delete_record", policy_lists::none,
        policy_lists::none, body_rule::none, policy_lists::none, policy_lists::none, body_rule::json,
        policy_lists::cloudflare_record_response, false},
    outbound_policy{outbound_operation::aliyun_describe_domains, "aliyun.describe_domains",
        policy_lists::aliyun_query, policy_lists::none, body_rule::none, policy_lists::none, policy_lists::none,
        body_rule::json, policy_lists::aliyun_response, false},
    outbound_policy{outbound_operation::aliyun_describe_domain_records, "aliyun.describe_domain_records",
        policy_lists::aliyun_query, policy_lists::none, body_rule::none, policy_lists::none, policy_lists::none,
        body_rule::json, policy_lists::aliyun_response, false},
    outbound_policy{outbound_operation::aliyun_describe_support_lines, "aliyun.describe_support_lines",
        policy_lists::aliyun_query, policy_lists::none, body_rule::none, policy_lists::none, policy_lists::none,
        body_rule::json, policy_lists::aliyun_response, false},
    outbound_policy{outbound_operation::aliyun_add_domain_record, "aliyun.add_domain_record",
        policy_lists::aliyun_query, policy_lists::none, body_rule::none, policy_lists::none, policy_lists::none,
        body_rule::json, policy_lists::aliyun_response, false},
    outbound_policy{outbound_operation::aliyun_update_domain_record, "aliyun.update_domain_record",
        policy_lists::aliyun_query, policy_lists::none, body_rule::none, policy_lists::none, policy_lists::none,
        body_rule::json, policy_lists::aliyun_response, false},
    outbound_policy{outbound_operation::aliyun_delete_domain_record, "aliyun.delete_domain_record",
        policy_lists::aliyun_query, policy_lists::none, body_rule::none, policy_lists::none, policy_lists::none,
        body_rule::json, policy_lists::aliyun_response, false},
    outbound_policy{outbound_operation::acme_directory, "acme.directory", policy_lists::none, policy_lists::none,
        body_rule::none, policy_lists::none, policy_lists::none, body_rule::json,
        policy_lists::acme_directory_response, false},
    outbound_policy{outbound_operation::acme_new_nonce, "acme.new_nonce", policy_lists::none, policy_lists::none,
        body_rule::none, policy_lists::none, policy_lists::none, body_rule::json, policy_lists::acme_problem_response,
        false},
    outbound_policy{outbound_operation::acme_new_account, "acme.new_account", policy_lists::none,
        policy_lists::none, body_rule::jws, policy_lists::acme_account_request, policy_lists::none,
        body_rule::json, policy_lists::acme_account_response, false},
    outbound_policy{outbound_operation::acme_new_order, "acme.new_order", policy_lists::none, policy_lists::none,
        body_rule::jws, policy_lists::acme_order_request, policy_lists::none, body_rule::json,
        policy_lists::acme_order_response, false},
    outbound_policy{outbound_operation::acme_authorization, "acme.authorization", policy_lists::none,
        policy_lists::none, body_rule::jws, policy_lists::none, policy_lists::none, body_rule::json,
        policy_lists::acme_authorization_response, false},
    outbound_policy{outbound_operation::acme_challenge, "acme.challenge", policy_lists::none, policy_lists::none,
        body_rule::jws, policy_lists::none, policy_lists::none, body_rule::json,
        policy_lists::acme_challenge_response, false},
    outbound_policy{outbound_operation::acme_finalize, "acme.finalize", policy_lists::none, policy_lists::none,
        body_rule::jws, policy_lists::none, policy_lists::acme_csr_field, body_rule::json,
        policy_lists::acme_order_response, false},
    outbound_policy{outbound_operation::acme_order, "acme.order", policy_lists::none, policy_lists::none,
        body_rule::jws, policy_lists::none, policy_lists::none, body_rule::json, policy_lists::acme_order_response,
        false},
    outbound_policy{outbound_operation::acme_certificate, "acme.certificate", policy_lists::none,
        policy_lists::none, body_rule::jws, policy_lists::none, policy_lists::none, body_rule::pem_chain,
        policy_lists::acme_problem_response, false},
    outbound_policy{outbound_operation::zerossl_eab_by_email, "zerossl.eab_by_email", policy_lists::none,
        policy_lists::none, body_rule::form, policy_lists::none, policy_lists::none, body_rule::json,
        policy_lists::zerossl_eab_response, false},
    outbound_policy{outbound_operation::zerossl_eab_by_access_key, "zerossl.eab_by_access_key", policy_lists::none,
        policy_lists::none, body_rule::none, policy_lists::none, policy_lists::none, body_rule::json,
        policy_lists::zerossl_eab_response, false},
    outbound_policy{outbound_operation::doh_resolve, "doh.resolve", policy_lists::doh_query, policy_lists::none,
        body_rule::none, policy_lists::none, policy_lists::none, body_rule::json, policy_lists::doh_response, true},
};

[[nodiscard]] inline constexpr const outbound_policy& policy_for(outbound_operation operation) noexcept {
    for (const auto& policy : outbound_policies) {
        if (policy.operation_ == operation) {
            return policy;
        }
    }
    return outbound_policies.front();
}

// What a call site tells the recorder: the configured origin (scheme and host
// come from the outbound configuration, never from the request), the
// operation that selects the allow-lists, and an optional subject.
struct outbound_call final {
    std::string_view origin_alias_;
    outbound_operation operation_;
    std::string_view subject_{};
};

}  // namespace service::attempt_trace
