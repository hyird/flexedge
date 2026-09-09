#pragma once

#include <ruvia/web/Model.h>

namespace service::certificate_issuance {

RUVIA_REQUEST_MODEL(CertificateProviderAccessKeyInput,
                    RUVIA_OPTIONAL_FIELD(envelope, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(hint, ruvia::String));
RUVIA_REQUEST_MODEL(CertificateProviderConfigInput,
                    RUVIA_OPTIONAL_FIELD_NAME("credential_mode", credentialMode, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("account_email", accountEmail, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("access_key", accessKey,
                                              CertificateProviderAccessKeyInput));
RUVIA_RESPONSE_MODEL(CertificateProviderAccessKeyOutput,
                     RUVIA_REQUIRED_FIELD(envelope, ruvia::String),
                     RUVIA_REQUIRED_FIELD(hint, ruvia::String));
RUVIA_RESPONSE_MODEL(CertificateProviderConfigOutput,
                     RUVIA_REQUIRED_FIELD_NAME("credential_mode", credentialMode, ruvia::String),
                     RUVIA_OPTIONAL_FIELD_NAME("account_email", accountEmail, ruvia::String,
                                               RUVIA_OMIT_EMPTY),
                     RUVIA_OPTIONAL_FIELD_NAME("access_key", accessKey,
                                               CertificateProviderAccessKeyOutput,
                                               RUVIA_OMIT_EMPTY));
RUVIA_REQUEST_MODEL(CertificateProviderEabRuntimeInput, RUVIA_OPTIONAL_FIELD(kid, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("hmac_key_envelope", hmacKeyEnvelope, ruvia::String));
RUVIA_REQUEST_MODEL(CertificateProviderAccountRuntimeInput,
                    RUVIA_OPTIONAL_FIELD_NAME("private_key_envelope", privateKeyEnvelope,
                                              ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("account_url", accountUrl, ruvia::String));
RUVIA_REQUEST_MODEL(CertificateProviderRuntimeInput,
                    RUVIA_OPTIONAL_FIELD(eab, CertificateProviderEabRuntimeInput),
                    RUVIA_OPTIONAL_FIELD_NAME("acme_account", acmeAccount,
                                              CertificateProviderAccountRuntimeInput));
RUVIA_RESPONSE_MODEL(CertificateProviderEabRuntimeOutput, RUVIA_REQUIRED_FIELD(kid, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("hmac_key_envelope", hmacKeyEnvelope,
                                               ruvia::String));
RUVIA_RESPONSE_MODEL(CertificateProviderAccountRuntimeOutput,
                     RUVIA_REQUIRED_FIELD_NAME("private_key_envelope", privateKeyEnvelope,
                                               ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("account_url", accountUrl, ruvia::String));
RUVIA_RESPONSE_MODEL(CertificateProviderRuntimeOutput,
                     RUVIA_OPTIONAL_FIELD(eab, CertificateProviderEabRuntimeOutput,
                                          RUVIA_OMIT_EMPTY),
                     RUVIA_OPTIONAL_FIELD_NAME("acme_account", acmeAccount,
                                               CertificateProviderAccountRuntimeOutput,
                                               RUVIA_OMIT_EMPTY));


} // namespace service::certificate_issuance
