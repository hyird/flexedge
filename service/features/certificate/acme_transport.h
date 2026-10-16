#pragma once

#include <array>
#include <string_view>

#include "ruvia/web/model.h"

#include "service/features/attempt_trace/outbound_policy.h"

namespace service::certificate_issuance {

// Trace policies of the ACME operations (RFC 8555).
namespace acme_trace {

using namespace std::string_view_literals;
using service::attempt_trace::body_rule;
using service::attempt_trace::outbound_policy;

inline constexpr std::array request_id_headers{"x-request-id"sv};
// A problem document names its type; a failed order or challenge its error's.
inline constexpr std::array error_code_fields{"type"sv, "error.type"sv};
inline constexpr service::attempt_trace::reply_identity identity{.request_id_headers_ = request_id_headers,
    .error_code_fields_ = error_code_fields,
    .error_code_on_failure_only_ = true};

inline constexpr std::array directory_response{"newNonce"sv, "newAccount"sv, "newOrder"sv, "revokeCert"sv,
    "keyChange"sv, "renewalInfo"sv, "meta.termsOfService"sv, "meta.website"sv, "meta.caaIdentities[]"sv,
    "meta.externalAccountRequired"sv, "type"sv, "detail"sv, "status"sv};
inline constexpr std::array account_request{"termsOfServiceAgreed"sv, "onlyReturnExisting"sv};
inline constexpr std::array account_response{"status"sv, "orders"sv, "createdAt"sv, "type"sv, "detail"sv,
    "subproblems[].type"sv, "subproblems[].detail"sv, "subproblems[].identifier.type"sv,
    "subproblems[].identifier.value"sv};
inline constexpr std::array order_request{"identifiers[].type"sv, "identifiers[].value"sv};
inline constexpr std::array order_response{"status"sv, "expires"sv, "notBefore"sv, "notAfter"sv,
    "identifiers[].type"sv, "identifiers[].value"sv, "authorizations[]"sv, "finalize"sv, "certificate"sv,
    "error.type"sv, "error.detail"sv, "error.status"sv, "error.subproblems[].type"sv,
    "error.subproblems[].detail"sv, "error.subproblems[].identifier.type"sv,
    "error.subproblems[].identifier.value"sv, "type"sv, "detail"sv, "subproblems[].type"sv,
    "subproblems[].detail"sv, "subproblems[].identifier.type"sv, "subproblems[].identifier.value"sv};
inline constexpr std::array authorization_response{"status"sv, "expires"sv, "wildcard"sv, "identifier.type"sv,
    "identifier.value"sv, "challenges[].type"sv, "challenges[].status"sv, "challenges[].url"sv,
    "challenges[].validated"sv, "challenges[].error.type"sv, "challenges[].error.detail"sv,
    "challenges[].error.status"sv, "type"sv, "detail"sv, "subproblems[].type"sv, "subproblems[].detail"sv};
inline constexpr std::array challenge_response{"type"sv, "status"sv, "url"sv, "validated"sv, "error.type"sv,
    "error.detail"sv, "error.status"sv, "detail"sv, "subproblems[].type"sv, "subproblems[].detail"sv};
inline constexpr std::array problem_response{"type"sv, "detail"sv, "status"sv, "subproblems[].type"sv,
    "subproblems[].detail"sv, "subproblems[].identifier.type"sv, "subproblems[].identifier.value"sv};
inline constexpr std::array csr_field{"csr"sv};

inline constexpr outbound_policy directory{.code_ = "acme.directory",
    .response_body_ = body_rule::json,
    .response_fields_ = directory_response,
    .identity_ = identity};
inline constexpr outbound_policy new_nonce{.code_ = "acme.new_nonce",
    .response_body_ = body_rule::json,
    .response_fields_ = problem_response,
    .identity_ = identity};
inline constexpr outbound_policy new_account{.code_ = "acme.new_account",
    .request_body_ = body_rule::jws,
    .request_fields_ = account_request,
    .response_body_ = body_rule::json,
    .response_fields_ = account_response,
    .identity_ = identity};
inline constexpr outbound_policy new_order{.code_ = "acme.new_order",
    .request_body_ = body_rule::jws,
    .request_fields_ = order_request,
    .response_body_ = body_rule::json,
    .response_fields_ = order_response,
    .identity_ = identity};
inline constexpr outbound_policy authorization{.code_ = "acme.authorization",
    .request_body_ = body_rule::jws,
    .response_body_ = body_rule::json,
    .response_fields_ = authorization_response,
    .identity_ = identity};
inline constexpr outbound_policy challenge{.code_ = "acme.challenge",
    .request_body_ = body_rule::jws,
    .response_body_ = body_rule::json,
    .response_fields_ = challenge_response,
    .identity_ = identity};
inline constexpr outbound_policy finalize{.code_ = "acme.finalize",
    .request_body_ = body_rule::jws,
    .request_sized_fields_ = csr_field,
    .response_body_ = body_rule::json,
    .response_fields_ = order_response,
    .identity_ = identity};
inline constexpr outbound_policy order{.code_ = "acme.order",
    .request_body_ = body_rule::jws,
    .response_body_ = body_rule::json,
    .response_fields_ = order_response,
    .identity_ = identity};
inline constexpr outbound_policy certificate{.code_ = "acme.certificate",
    .request_body_ = body_rule::jws,
    .response_body_ = body_rule::pem_chain,
    .response_fields_ = problem_response,
    .identity_ = identity};

}  // namespace acme_trace

RUVIA_MODEL(acme_directory_input,
    RUVIA_OPTIONAL_FIELD_NAME("newNonce", new_nonce, ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("newAccount", new_account, ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("newOrder", new_order, ruvia::string));
RUVIA_MODEL(acme_problem_input, RUVIA_OPTIONAL_FIELD(type, ruvia::string),
    RUVIA_OPTIONAL_FIELD(detail, ruvia::string),
    RUVIA_OPTIONAL_FIELD(status, ruvia::int64));
RUVIA_MODEL(acme_identifier_input, RUVIA_OPTIONAL_FIELD(type, ruvia::string),
    RUVIA_OPTIONAL_FIELD(value, ruvia::string));
RUVIA_MODEL(acme_challenge_input, RUVIA_OPTIONAL_FIELD(type, ruvia::string),
    RUVIA_OPTIONAL_FIELD(url, ruvia::string),
    RUVIA_OPTIONAL_FIELD(status, ruvia::string),
    RUVIA_OPTIONAL_FIELD(token, ruvia::string),
    RUVIA_OPTIONAL_FIELD(error, acme_problem_input));
RUVIA_MODEL(acme_authorization_input, RUVIA_OPTIONAL_FIELD(status, ruvia::string),
    RUVIA_OPTIONAL_FIELD(identifier, acme_identifier_input),
    RUVIA_OPTIONAL_FIELD(challenges, ruvia::array<acme_challenge_input>));
RUVIA_MODEL(acme_order_input, RUVIA_OPTIONAL_FIELD(status, ruvia::string),
    RUVIA_OPTIONAL_FIELD(authorizations, ruvia::array<ruvia::string>),
    RUVIA_OPTIONAL_FIELD(finalize, ruvia::string),
    RUVIA_OPTIONAL_FIELD(certificate, ruvia::string),
    RUVIA_OPTIONAL_FIELD(error, acme_problem_input));

RUVIA_MODEL(acme_jwk_output, RUVIA_REQUIRED_FIELD(e, ruvia::string),
    RUVIA_REQUIRED_FIELD(kty, ruvia::string),
    RUVIA_REQUIRED_FIELD(n, ruvia::string));
RUVIA_MODEL(acme_protected_jwk_output, RUVIA_REQUIRED_FIELD(alg, ruvia::string),
    RUVIA_REQUIRED_FIELD(jwk, acme_jwk_output),
    RUVIA_REQUIRED_FIELD(nonce, ruvia::string),
    RUVIA_REQUIRED_FIELD(url, ruvia::string));
RUVIA_MODEL(acme_protected_kid_output, RUVIA_REQUIRED_FIELD(alg, ruvia::string),
    RUVIA_REQUIRED_FIELD(kid, ruvia::string),
    RUVIA_REQUIRED_FIELD(nonce, ruvia::string),
    RUVIA_REQUIRED_FIELD(url, ruvia::string));
RUVIA_MODEL(acme_jws_output,
    RUVIA_REQUIRED_FIELD_NAME("protected", protected_value, ruvia::string),
    RUVIA_REQUIRED_FIELD(payload, ruvia::string),
    RUVIA_REQUIRED_FIELD(signature, ruvia::string));
RUVIA_MODEL(acme_eab_protected_output, RUVIA_REQUIRED_FIELD(alg, ruvia::string),
    RUVIA_REQUIRED_FIELD(kid, ruvia::string),
    RUVIA_REQUIRED_FIELD(url, ruvia::string));
RUVIA_MODEL(acme_account_payload_output,
    RUVIA_REQUIRED_FIELD_NAME("termsOfServiceAgreed", terms_agreed, ruvia::bool_value),
    RUVIA_OPTIONAL_FIELD(contact, ruvia::array<ruvia::string>, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("externalAccountBinding", external_account_binding,
        acme_jws_output));
RUVIA_MODEL(acme_identifier_output, RUVIA_REQUIRED_FIELD(type, ruvia::string),
    RUVIA_REQUIRED_FIELD(value, ruvia::string));
RUVIA_MODEL(acme_order_payload_output,
    RUVIA_REQUIRED_FIELD(identifiers, ruvia::array<acme_identifier_output>));
RUVIA_MODEL(acme_finalize_payload_output, RUVIA_REQUIRED_FIELD(csr, ruvia::string));
RUVIA_MODEL(acme_empty_payload_output,
    RUVIA_OPTIONAL_FIELD(unused, ruvia::string, RUVIA_OMIT_EMPTY));

}  // namespace service::certificate_issuance
