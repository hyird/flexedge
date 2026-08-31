import type { RevisionedResourceRef } from '@/utils/http';
import type { LogTail, LogTailLimit } from '@/utils/log_tail';
import type { PageParams } from '@/utils/pagination.types';
import { createQueryKeys } from '@/utils/query.keys';

export type WebsiteStatus = 'enabled' | 'disabled';
export type WebsiteDeployStatus = 'no_nodes' | 'pending' | 'partial' | 'applied';
export type WebsiteResolutionStatus = 'unverified' | 'verified' | 'invalid';
export type WebsiteDnsMode = 'managed' | 'external';
export type WebsiteOriginProtocol = 'http' | 'https';
export type WebsiteOriginRole = 'primary' | 'backup';
export type WebsiteRouteMatchType = 'exact' | 'prefix';
export type WebsiteRouteAction = 'proxy' | 'redirect';
export type WebsiteTlsVersion = '1.2' | '1.3';
export type WebsiteAccessLogStatusCodeRange = '1xx' | '2xx' | '3xx' | '4xx' | '5xx';

export interface WebsiteWire {
    id: string;
    cluster_id: string;
    cluster_name: string;
    access_domain: string;
    status: WebsiteStatus;
    revision: number;
    created_at: string;
    updated_at: string;
    config: WebsiteConfig;
    certificates: WebsiteCertificate[];
    runtime: WebsiteRuntime;
}

export interface WebsiteCertificate {
    id: string;
    domains: string[];
    usable: boolean;
}

export interface WebsiteItem extends WebsiteWire {
    website_name: string;
    origin_count: number;
    https_enabled: boolean;
    deploy_status: WebsiteDeployStatus;
    target_node_count: number;
    synced_node_count: number;
}

export interface WebsiteConfig {
    name?: string;
    domains: Array<Pick<WebsiteDomainItem, 'id' | 'hostname' | 'dns_mode'>>;
    origins: WebsiteOriginItem[];
    default_origin_group: string;
    origin_host_header: string;
    origin_connect_timeout_seconds: number;
    origin_read_timeout_seconds: number;
    pass_client_ip: boolean;
    health_check_enabled: boolean;
    health_check_path: string;
    health_check_interval_seconds: number;
    health_check_timeout_seconds: number;
    health_check_expected_status: number;
    healthy_threshold: number;
    unhealthy_threshold: number;
    access_log_enabled: boolean;
    access_log_request_headers: boolean;
    access_log_request_body: boolean;
    access_log_response_headers: boolean;
    access_log_query_params: boolean;
    access_log_cookies: boolean;
    access_log_referer: boolean;
    access_log_user_agent: boolean;
    access_log_status_code_ranges: WebsiteAccessLogStatusCodeRange[];
    access_log_client_abort: boolean;
    https_enabled: boolean;
    certificate_ids: string[];
    minimum_tls_version: WebsiteTlsVersion;
    force_https: boolean;
    http2_enabled: boolean;
    hsts_enabled: boolean;
    response_compression_enabled: boolean;
    response_compression_min_bytes: number;
    response_compression_max_bytes: number;
    response_compression_algorithms: string[];
    response_compression_mime_types: string[];
    response_compression_extensions: string[];
    response_compression_excluded_extensions: string[];
    route_rules: WebsiteRouteRule[];
}

export interface WebsiteRuntime {
    domain_states: Array<{
        id: string;
        access_protocol: WebsiteOriginProtocol;
        resolution_status: WebsiteResolutionStatus;
        last_verified_at?: string;
        last_error?: string;
    }>;
    origin_states: Array<{
        node_id: string;
        node_name: string;
        origin_id: string;
        status: string;
        checked_at_unix_millis: number;
        latency_millis: number;
        last_error?: string;
    }>;
    deploy_status: WebsiteDeployStatus;
    target_node_count: number;
    synced_node_count: number;
}

export interface WebsiteDomainItem {
    id: string;
    hostname: string;
    dns_mode: WebsiteDnsMode;
    access_protocol: WebsiteOriginProtocol;
    resolution_status: WebsiteResolutionStatus;
    last_verified_at?: string;
    last_error?: string;
}

export interface WebsiteOriginItem {
    id: string;
    group: string;
    protocol: WebsiteOriginProtocol;
    host: string;
    port: number;
    role: WebsiteOriginRole;
    weight: number;
    status: WebsiteStatus;
}

export interface WebsiteRouteHeader {
    name: string;
    value: string;
}

export interface WebsiteRouteRule {
    id: string;
    status: WebsiteStatus;
    match_type: WebsiteRouteMatchType;
    path: string;
    methods: string[];
    action: WebsiteRouteAction;
    rewrite_path: string;
    redirect_url: string;
    redirect_status: 0 | 301 | 302;
    origin_group: string;
    request_headers: WebsiteRouteHeader[];
    response_headers: WebsiteRouteHeader[];
}

export interface WebsiteAccessLog {
    id: string;
    occurred_at: string;
    node_id: string;
    node_name: string;
    client_ip?: string;
    protocol: string;
    method: string;
    host: string;
    target: string;
    status_code: number;
    response_bytes: number;
    duration_ms: number;
    user_agent?: string;
    referer?: string;
    request_headers?: string;
    request_body?: string;
    request_body_truncated: boolean;
    tls_fingerprint?: string;
    response_headers?: string;
    query_string?: string;
    cookies?: string;
}

export type WebsiteAccessLogLimit = LogTailLimit;
export type WebsiteAccessLogTail = LogTail<WebsiteAccessLog>;

export type WebsiteDetail = WebsiteItem;

export interface WebsiteQuery extends PageParams {
    clusterId?: string;
    status?: WebsiteStatus;
}

export interface WebsiteSaveInput {
    status: WebsiteStatus;
    config: WebsiteConfig;
}

export interface WebsiteSaveTarget extends RevisionedResourceRef {
    cluster_id: string;
}

export const websiteQueryKeys = createQueryKeys('websites');
