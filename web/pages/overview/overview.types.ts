export interface OverviewResourceCounts {
    website_count: number;
    domain_count: number;
    certificate_count: number;
    cluster_count: number;
}

export interface OverviewIssueCounts {
    dns_zone_issue_count: number;
    certificate_expiring_count: number;
    certificate_failed_count: number;
    active_task_count: number;
    failed_task_count: number;
}

export interface OverviewTask {
    id: string;
    kind: 'provider' | 'dns' | 'certificate' | 'website';
    resource_type: 'provider' | 'dns_zone' | 'certificate' | 'website';
    resource_id: string;
    resource_name: string;
    operation: string;
    status: 'pending' | 'running' | 'retry' | 'completed';
    last_error?: string;
    updated_at: string;
}

export interface OverviewData {
    resources: OverviewResourceCounts;
    issues: OverviewIssueCounts;
    recent_tasks: OverviewTask[];
}

export const overviewQueryKeys = {
    all: ['overview'] as const,
};
