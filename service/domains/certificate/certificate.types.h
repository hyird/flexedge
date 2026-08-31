#pragma once

#include <ruvia/web/Model.h>

#include "service/features/certificate/model.h"

namespace service::certificate {

RUVIA_REQUEST_MODEL(CreateCertificateBody, RUVIA_OPTIONAL_FIELD(domain, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("certificate_provider_id", certificateProviderId,
                                              ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("dns_zone_id", dnsZoneId, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(config,
                                         service::certificate_issuance::CertificateConfigInput));
RUVIA_RESPONSE_MODEL(
    CertificateDto, RUVIA_REQUIRED_FIELD(id, ruvia::String),
    RUVIA_REQUIRED_FIELD(revision, ruvia::Int64),
    RUVIA_REQUIRED_FIELD(domains, ruvia::Array<ruvia::String>),
    RUVIA_REQUIRED_FIELD(issuer, ruvia::String),
    RUVIA_REQUIRED_FIELD_NAME("certificate_provider_id", certificateProviderId, ruvia::String),
    RUVIA_REQUIRED_FIELD_NAME("certificate_provider", certificateProvider, ruvia::String),
    RUVIA_REQUIRED_FIELD(status, ruvia::String), RUVIA_REQUIRED_FIELD(usable, ruvia::Bool),
    RUVIA_REQUIRED_FIELD(config, service::certificate_issuance::CertificateConfigOutput),
    RUVIA_REQUIRED_FIELD_NAME("dns_zone_id", dnsZoneId, ruvia::String),
    RUVIA_REQUIRED_FIELD_NAME("dns_zone_domain", dnsZoneDomain, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("not_before", notBefore, ruvia::String, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("expires_at", expiresAt, ruvia::String, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("remaining_days", remainingDays, ruvia::Int64, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("last_error", lastError, ruvia::String, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("serial_number", serialNumber, ruvia::String, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("fingerprint_sha256", fingerprintSha256, ruvia::String,
                              RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("last_issued_at", lastIssuedAt, ruvia::String, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("sync_status", syncStatus, ruvia::String, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("sync_count_fails", syncCountFails, ruvia::Int64, RUVIA_OMIT_EMPTY),
    RUVIA_REQUIRED_FIELD_NAME("website_count", websiteCount, ruvia::Int64),
    RUVIA_REQUIRED_FIELD_NAME("created_at", createdAt, ruvia::String),
    RUVIA_REQUIRED_FIELD_NAME("updated_at", updatedAt, ruvia::String));
RUVIA_RESPONSE_MODEL(CertificatePageDataDto,
                     RUVIA_REQUIRED_FIELD(list, ruvia::Array<CertificateDto>),
                     RUVIA_REQUIRED_FIELD(total, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD(page, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD_NAME("page_size", pageSize, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD_NAME("total_pages", totalPages, ruvia::Int64));
RUVIA_RESPONSE_MODEL(CertificatePageResponse, RUVIA_REQUIRED_FIELD(code, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD(message, ruvia::String),
                     RUVIA_REQUIRED_FIELD(data, CertificatePageDataDto));
RUVIA_RESPONSE_MODEL(CertificateDetailResponse, RUVIA_REQUIRED_FIELD(code, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD(message, ruvia::String),
                     RUVIA_REQUIRED_FIELD(data, CertificateDto));
} // namespace service::certificate
