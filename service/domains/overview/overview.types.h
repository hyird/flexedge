#pragma once

#include <ruvia/web/Model.h>

namespace service::overview {

RUVIA_RESPONSE_MODEL(OverviewResourceCountsDto,
                     RUVIA_REQUIRED_FIELD_NAME("website_count", websiteCount, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD_NAME("domain_count", domainCount, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD_NAME("certificate_count", certificateCount, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD_NAME("cluster_count", clusterCount, ruvia::Int64));

RUVIA_RESPONSE_MODEL(
    OverviewIssueCountsDto,
    RUVIA_REQUIRED_FIELD_NAME("dns_zone_issue_count", dnsZoneIssueCount, ruvia::Int64),
    RUVIA_REQUIRED_FIELD_NAME("certificate_expiring_count", certificateExpiringCount, ruvia::Int64),
    RUVIA_REQUIRED_FIELD_NAME("certificate_failed_count", certificateFailedCount, ruvia::Int64),
    RUVIA_REQUIRED_FIELD_NAME("active_task_count", activeTaskCount, ruvia::Int64),
    RUVIA_REQUIRED_FIELD_NAME("failed_task_count", failedTaskCount, ruvia::Int64));

RUVIA_RESPONSE_MODEL(OverviewTaskDto, RUVIA_REQUIRED_FIELD(id, ruvia::String),
                     RUVIA_REQUIRED_FIELD(kind, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("resource_type", resourceType, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("resource_id", resourceId, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("resource_name", resourceName, ruvia::String),
                     RUVIA_REQUIRED_FIELD(operation, ruvia::String),
                     RUVIA_REQUIRED_FIELD(status, ruvia::String),
                     RUVIA_OPTIONAL_FIELD_NAME("last_error", lastError, ruvia::String,
                                               RUVIA_OMIT_EMPTY),
                     RUVIA_REQUIRED_FIELD_NAME("updated_at", updatedAt, ruvia::String));

RUVIA_RESPONSE_MODEL(OverviewDataDto, RUVIA_REQUIRED_FIELD(resources, OverviewResourceCountsDto),
                     RUVIA_REQUIRED_FIELD(issues, OverviewIssueCountsDto),
                     RUVIA_REQUIRED_FIELD_NAME("recent_tasks", recentTasks,
                                               ruvia::Array<OverviewTaskDto>));

RUVIA_RESPONSE_MODEL(OverviewResponse, RUVIA_REQUIRED_FIELD(code, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD(message, ruvia::String),
                     RUVIA_REQUIRED_FIELD(data, OverviewDataDto));

} // namespace service::overview
