#pragma once

#include "ruvia/web/model.h"

namespace service::certificate_issuance {

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
