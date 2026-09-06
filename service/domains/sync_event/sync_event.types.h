#pragma once

#include <ruvia/web/Model.h>

namespace service::sync_event {

RUVIA_RESPONSE_MODEL(SyncEventDto, RUVIA_REQUIRED_FIELD(sequence, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD_NAME("resource_type", resourceType, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("resource_id", resourceId, ruvia::String),
                     RUVIA_REQUIRED_FIELD(operation, ruvia::String),
                     RUVIA_REQUIRED_FIELD(version, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD(outcome, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("emitted_at", emittedAt, ruvia::String));

RUVIA_RESPONSE_MODEL(SyncEventPageDataDto, RUVIA_REQUIRED_FIELD(list, ruvia::Array<SyncEventDto>),
                     RUVIA_REQUIRED_FIELD(cursor, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD_NAME("has_more", hasMore, ruvia::Bool));

RUVIA_RESPONSE_MODEL(SyncEventPageResponse, RUVIA_REQUIRED_FIELD(code, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD(message, ruvia::String),
                     RUVIA_REQUIRED_FIELD(data, SyncEventPageDataDto));

} // namespace service::sync_event
