#pragma once

#include <ruvia/web/Model.h>

namespace service::dns_sync {

RUVIA_REQUEST_MODEL(ZoneRecordInput, RUVIA_OPTIONAL_FIELD(id, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(type, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(name, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(content, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(ttl, ruvia::Int64),
                    RUVIA_OPTIONAL_FIELD(priority, ruvia::Int64),
                    RUVIA_OPTIONAL_FIELD(proxied, ruvia::Bool),
                    RUVIA_OPTIONAL_FIELD_NAME("line_code", lineCode, ruvia::String));

RUVIA_REQUEST_MODEL(ZoneConfigInput, RUVIA_OPTIONAL_FIELD(records, ruvia::Array<ZoneRecordInput>));

RUVIA_RESPONSE_MODEL(ZoneRecordOutput, RUVIA_REQUIRED_FIELD(id, ruvia::String),
                     RUVIA_REQUIRED_FIELD(type, ruvia::String),
                     RUVIA_REQUIRED_FIELD(name, ruvia::String),
                     RUVIA_REQUIRED_FIELD(content, ruvia::String),
                     RUVIA_REQUIRED_FIELD(ttl, ruvia::Int64),
                     RUVIA_OPTIONAL_FIELD(priority, ruvia::Int64, RUVIA_OMIT_EMPTY),
                     RUVIA_REQUIRED_FIELD(proxied, ruvia::Bool),
                     RUVIA_REQUIRED_FIELD_NAME("line_code", lineCode, ruvia::String));

RUVIA_RESPONSE_MODEL(ZoneConfigOutput,
                     RUVIA_REQUIRED_FIELD(records, ruvia::Array<ZoneRecordOutput>));

RUVIA_REQUEST_MODEL(ZoneLineRuntimeInput, RUVIA_OPTIONAL_FIELD(code, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(name, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("display_name", displayName, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(status, ruvia::String));

RUVIA_REQUEST_MODEL(ZoneRecordRuntimeInput, RUVIA_OPTIONAL_FIELD(id, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("remote_record_id", remoteRecordId, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("sync_status", syncStatus, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("synced_revision", syncedRevision, ruvia::Int64),
                    RUVIA_OPTIONAL_FIELD_NAME("last_error", lastError, ruvia::String));

RUVIA_REQUEST_MODEL(ZoneRecordConflictInput, RUVIA_OPTIONAL_FIELD(id, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(type, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(name, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("line_code", lineCode, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("local_content", localContent, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("remote_content", remoteContent, ruvia::String));

RUVIA_REQUEST_MODEL(ZoneChallengeRuntimeInput, RUVIA_OPTIONAL_FIELD(id, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(name, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(content, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(ttl, ruvia::Int64),
                    RUVIA_OPTIONAL_FIELD_NAME("certificate_id", certificateId, ruvia::String));

RUVIA_REQUEST_MODEL(ZoneRuntimeInput,
                    RUVIA_OPTIONAL_FIELD_NAME("records_imported", recordsImported, ruvia::Bool),
                    RUVIA_OPTIONAL_FIELD_NAME("lines_synced_at", linesSyncedAt, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(lines, ruvia::Array<ZoneLineRuntimeInput>),
                    RUVIA_OPTIONAL_FIELD_NAME("record_states", recordStates,
                                              ruvia::Array<ZoneRecordRuntimeInput>),
                    RUVIA_OPTIONAL_FIELD(conflicts, ruvia::Array<ZoneRecordConflictInput>),
                    RUVIA_OPTIONAL_FIELD_NAME("challenge_records", challengeRecords,
                                              ruvia::Array<ZoneChallengeRuntimeInput>));

RUVIA_RESPONSE_MODEL(ZoneLineRuntimeDto, RUVIA_REQUIRED_FIELD(code, ruvia::String),
                     RUVIA_REQUIRED_FIELD(name, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("display_name", displayName, ruvia::String),
                     RUVIA_REQUIRED_FIELD(status, ruvia::String));

RUVIA_RESPONSE_MODEL(ZoneRecordRuntimeDto, RUVIA_REQUIRED_FIELD(id, ruvia::String),
                     RUVIA_OPTIONAL_FIELD_NAME("remote_record_id", remoteRecordId, ruvia::String,
                                               RUVIA_OMIT_EMPTY),
                     RUVIA_REQUIRED_FIELD_NAME("sync_status", syncStatus, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("synced_revision", syncedRevision, ruvia::Int64),
                     RUVIA_OPTIONAL_FIELD_NAME("last_error", lastError, ruvia::String,
                                               RUVIA_OMIT_EMPTY));

RUVIA_RESPONSE_MODEL(ZoneRecordConflictDto, RUVIA_REQUIRED_FIELD(id, ruvia::String),
                     RUVIA_REQUIRED_FIELD(type, ruvia::String),
                     RUVIA_REQUIRED_FIELD(name, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("line_code", lineCode, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("local_content", localContent, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("remote_content", remoteContent, ruvia::String));

RUVIA_RESPONSE_MODEL(ZoneChallengeRuntimeDto, RUVIA_REQUIRED_FIELD(id, ruvia::String),
                     RUVIA_REQUIRED_FIELD(name, ruvia::String),
                     RUVIA_REQUIRED_FIELD(content, ruvia::String),
                     RUVIA_REQUIRED_FIELD(ttl, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD_NAME("certificate_id", certificateId, ruvia::String));

RUVIA_RESPONSE_MODEL(
    ZoneRuntimeDto, RUVIA_REQUIRED_FIELD_NAME("records_imported", recordsImported, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("lines_synced_at", linesSyncedAt, ruvia::String, RUVIA_OMIT_EMPTY),
    RUVIA_REQUIRED_FIELD(lines, ruvia::Array<ZoneLineRuntimeDto>),
    RUVIA_REQUIRED_FIELD_NAME("record_states", recordStates, ruvia::Array<ZoneRecordRuntimeDto>),
    RUVIA_REQUIRED_FIELD(conflicts, ruvia::Array<ZoneRecordConflictDto>),
    RUVIA_OPTIONAL_FIELD_NAME("challenge_records", challengeRecords,
                              ruvia::Array<ZoneChallengeRuntimeDto>, RUVIA_OMIT_EMPTY));

} // namespace service::dns_sync
