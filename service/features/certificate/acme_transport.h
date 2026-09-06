#pragma once

#include <ruvia/web/Model.h>

namespace service::certificate_issuance {

RUVIA_REQUEST_MODEL(AcmeDirectoryInput,
                    RUVIA_OPTIONAL_FIELD_NAME("newNonce", newNonce, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("newAccount", newAccount, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("newOrder", newOrder, ruvia::String));
RUVIA_REQUEST_MODEL(AcmeProblemInput, RUVIA_OPTIONAL_FIELD(type, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(detail, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(status, ruvia::Int64));
RUVIA_REQUEST_MODEL(AcmeIdentifierInput, RUVIA_OPTIONAL_FIELD(type, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(value, ruvia::String));
RUVIA_REQUEST_MODEL(AcmeChallengeInput, RUVIA_OPTIONAL_FIELD(type, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(url, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(status, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(token, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(error, AcmeProblemInput));
RUVIA_REQUEST_MODEL(AcmeAuthorizationInput, RUVIA_OPTIONAL_FIELD(status, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(identifier, AcmeIdentifierInput),
                    RUVIA_OPTIONAL_FIELD(challenges, ruvia::Array<AcmeChallengeInput>));
RUVIA_REQUEST_MODEL(AcmeOrderInput, RUVIA_OPTIONAL_FIELD(status, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(authorizations, ruvia::Array<ruvia::String>),
                    RUVIA_OPTIONAL_FIELD(finalize, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(certificate, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(error, AcmeProblemInput));

RUVIA_RESPONSE_MODEL(AcmeJwkOutput, RUVIA_REQUIRED_FIELD(e, ruvia::String),
                     RUVIA_REQUIRED_FIELD(kty, ruvia::String),
                     RUVIA_REQUIRED_FIELD(n, ruvia::String));
RUVIA_RESPONSE_MODEL(AcmeProtectedJwkOutput, RUVIA_REQUIRED_FIELD(alg, ruvia::String),
                     RUVIA_REQUIRED_FIELD(jwk, AcmeJwkOutput),
                     RUVIA_REQUIRED_FIELD(nonce, ruvia::String),
                     RUVIA_REQUIRED_FIELD(url, ruvia::String));
RUVIA_RESPONSE_MODEL(AcmeProtectedKidOutput, RUVIA_REQUIRED_FIELD(alg, ruvia::String),
                     RUVIA_REQUIRED_FIELD(kid, ruvia::String),
                     RUVIA_REQUIRED_FIELD(nonce, ruvia::String),
                     RUVIA_REQUIRED_FIELD(url, ruvia::String));
RUVIA_RESPONSE_MODEL(AcmeJwsOutput,
                     RUVIA_REQUIRED_FIELD_NAME("protected", protectedValue, ruvia::String),
                     RUVIA_REQUIRED_FIELD(payload, ruvia::String),
                     RUVIA_REQUIRED_FIELD(signature, ruvia::String));
RUVIA_RESPONSE_MODEL(AcmeEabProtectedOutput, RUVIA_REQUIRED_FIELD(alg, ruvia::String),
                     RUVIA_REQUIRED_FIELD(kid, ruvia::String),
                     RUVIA_REQUIRED_FIELD(url, ruvia::String));
RUVIA_RESPONSE_MODEL(AcmeAccountPayloadOutput,
                     RUVIA_REQUIRED_FIELD_NAME("termsOfServiceAgreed", termsAgreed, ruvia::Bool),
                     RUVIA_OPTIONAL_FIELD(contact, ruvia::Array<ruvia::String>, RUVIA_OMIT_EMPTY),
                     RUVIA_OPTIONAL_FIELD_NAME("externalAccountBinding", externalAccountBinding,
                                               AcmeJwsOutput));
RUVIA_RESPONSE_MODEL(AcmeIdentifierOutput, RUVIA_REQUIRED_FIELD(type, ruvia::String),
                     RUVIA_REQUIRED_FIELD(value, ruvia::String));
RUVIA_RESPONSE_MODEL(AcmeOrderPayloadOutput,
                     RUVIA_REQUIRED_FIELD(identifiers, ruvia::Array<AcmeIdentifierOutput>));
RUVIA_RESPONSE_MODEL(AcmeFinalizePayloadOutput, RUVIA_REQUIRED_FIELD(csr, ruvia::String));
RUVIA_RESPONSE_MODEL(AcmeEmptyPayloadOutput,
                     RUVIA_OPTIONAL_FIELD(unused, ruvia::String, RUVIA_OMIT_EMPTY));

} // namespace service::certificate_issuance
