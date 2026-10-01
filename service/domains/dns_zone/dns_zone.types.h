#pragma once

#include "ruvia/web/Model.h"

#include <optional>
#include <string_view>

#include "service/features/dns_sync/transport.h"
#include "service/common/domain_name.h"
#include "service/common/uuid.h"

namespace service::dns_zone {

inline bool isValidUuid(const ruvia::String& value) {
    return service::common::parseUuid(std::optional<std::string_view>{value.view()}).has_value();
}

inline bool isValidZoneDomain(const ruvia::String& value) {
    return service::common::isHostname(value.view(), false);
}

RUVIA_MODEL(AvailableDnsZoneDto, RUVIA_REQUIRED_FIELD(domain, ruvia::String),
                     RUVIA_REQUIRED_FIELD(status, ruvia::String));
RUVIA_MODEL(AvailableDnsZoneListDataDto,
                     RUVIA_REQUIRED_FIELD(list, ruvia::Array<AvailableDnsZoneDto>));
RUVIA_MODEL(AvailableDnsZoneListResponse, RUVIA_REQUIRED_FIELD(code, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD(message, ruvia::String),
                     RUVIA_REQUIRED_FIELD(data, AvailableDnsZoneListDataDto));

RUVIA_MODEL(DnsZoneOptionDto, RUVIA_REQUIRED_FIELD(id, ruvia::String),
                     RUVIA_REQUIRED_FIELD(domain, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("dns_provider", dnsProvider, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("dns_provider_name", dnsProviderName, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("sync_status", syncStatus, ruvia::String),
                     RUVIA_REQUIRED_FIELD(available, ruvia::Bool));
RUVIA_MODEL(DnsZoneOptionListDataDto,
                     RUVIA_REQUIRED_FIELD(list, ruvia::Array<DnsZoneOptionDto>));
RUVIA_MODEL(DnsZoneOptionListResponse, RUVIA_REQUIRED_FIELD(code, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD(message, ruvia::String),
                     RUVIA_REQUIRED_FIELD(data, DnsZoneOptionListDataDto));
RUVIA_MODEL(DnsZoneCollectionResponse, RUVIA_REQUIRED_FIELD(code, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD(message, ruvia::String),
                     RUVIA_REQUIRED_FIELD(data, ruvia::Array<DnsZoneOptionDto>));

RUVIA_MODEL(CreateDnsZoneBody,
                    RUVIA_OPTIONAL_FIELD_NAME("dns_provider_id", dnsProviderId, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(domain, ruvia::String));
RUVIA_MODEL(DnsZoneSyncBody,
                    RUVIA_OPTIONAL_FIELD_NAME("conflict_policy", conflictPolicy, ruvia::String));

RUVIA_MODEL(ProjectedDnsRecordDto, RUVIA_REQUIRED_FIELD(id, ruvia::String),
                     RUVIA_REQUIRED_FIELD(type, ruvia::String),
                     RUVIA_REQUIRED_FIELD(name, ruvia::String),
                     RUVIA_REQUIRED_FIELD(content, ruvia::String),
                     RUVIA_REQUIRED_FIELD(ttl, ruvia::Int64),
                     RUVIA_OPTIONAL_FIELD(priority, ruvia::Int64, RUVIA_OMIT_EMPTY),
                     RUVIA_REQUIRED_FIELD(proxied, ruvia::Bool),
                     RUVIA_REQUIRED_FIELD_NAME("line_code", lineCode, ruvia::String));

RUVIA_MODEL(DnsLineRuntimeDto, RUVIA_REQUIRED_FIELD(code, ruvia::String),
                     RUVIA_REQUIRED_FIELD(name, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("display_name", displayName, ruvia::String),
                     RUVIA_REQUIRED_FIELD(status, ruvia::String));
RUVIA_MODEL(DnsRecordRuntimeDto, RUVIA_REQUIRED_FIELD(id, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("sync_status", syncStatus, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("synced_revision", syncedRevision, ruvia::Int64),
                     RUVIA_OPTIONAL_FIELD_NAME("last_error", lastError, ruvia::String,
                                               RUVIA_OMIT_EMPTY));
RUVIA_MODEL(DnsRecordConflictDto, RUVIA_REQUIRED_FIELD(id, ruvia::String),
                     RUVIA_REQUIRED_FIELD(type, ruvia::String),
                     RUVIA_REQUIRED_FIELD(name, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("line_code", lineCode, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("local_content", localContent, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("remote_content", remoteContent, ruvia::String));
RUVIA_MODEL(
    DnsZoneRuntimeDto, RUVIA_REQUIRED_FIELD_NAME("records_imported", recordsImported, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("lines_synced_at", linesSyncedAt, ruvia::String, RUVIA_OMIT_EMPTY),
    RUVIA_REQUIRED_FIELD(lines, ruvia::Array<DnsLineRuntimeDto>),
    RUVIA_REQUIRED_FIELD_NAME("projected_records", projectedRecords,
                              ruvia::Array<ProjectedDnsRecordDto>),
    RUVIA_REQUIRED_FIELD_NAME("record_states", recordStates, ruvia::Array<DnsRecordRuntimeDto>),
    RUVIA_REQUIRED_FIELD(conflicts, ruvia::Array<DnsRecordConflictDto>));

RUVIA_MODEL(DnsZoneDto, RUVIA_REQUIRED_FIELD(id, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("dns_provider_id", dnsProviderId, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("dns_provider", dnsProvider, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("dns_provider_name", dnsProviderName, ruvia::String),
                     RUVIA_REQUIRED_FIELD(domain, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("sync_status", syncStatus, ruvia::String),
                     RUVIA_REQUIRED_FIELD(revision, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD_NAME("desired_revision", desiredRevision, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD_NAME("synced_revision", syncedRevision, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD_NAME("website_count", websiteCount, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD(config, service::dns_sync::ZoneConfigOutput),
                     RUVIA_REQUIRED_FIELD(runtime, DnsZoneRuntimeDto),
                     RUVIA_OPTIONAL_FIELD_NAME("last_synced_at", lastSyncedAt, ruvia::String,
                                               RUVIA_OMIT_EMPTY),
                     RUVIA_OPTIONAL_FIELD_NAME("last_error", lastError, ruvia::String,
                                               RUVIA_OMIT_EMPTY),
                     RUVIA_REQUIRED_FIELD_NAME("created_at", createdAt, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("updated_at", updatedAt, ruvia::String));
RUVIA_MODEL(DnsZonePageDataDto, RUVIA_REQUIRED_FIELD(list, ruvia::Array<DnsZoneDto>),
                     RUVIA_REQUIRED_FIELD(total, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD(page, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD_NAME("page_size", pageSize, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD_NAME("total_pages", totalPages, ruvia::Int64));
RUVIA_MODEL(DnsZonePageResponse, RUVIA_REQUIRED_FIELD(code, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD(message, ruvia::String),
                     RUVIA_REQUIRED_FIELD(data, DnsZonePageDataDto));
RUVIA_MODEL(DnsZoneDetailResponse, RUVIA_REQUIRED_FIELD(code, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD(message, ruvia::String),
                     RUVIA_REQUIRED_FIELD(data, DnsZoneDto));

} // namespace service::dns_zone
