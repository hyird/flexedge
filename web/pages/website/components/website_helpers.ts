import { createUuid } from '@/utils/uuid';
import type { WebsiteCreateValues } from '../website.schema';
import type { WebsiteItem, WebsiteSaveInput, WebsiteStatus } from '../website.types';

export type StatusTone = 'success' | 'warning' | 'destructive' | 'info' | 'neutral';

export const websiteStatusMeta: Record<WebsiteStatus, { tone: StatusTone; label: string }> = {
    enabled: { tone: 'success', label: '已启用' },
    disabled: { tone: 'neutral', label: '已暂停' },
};

export const resolutionStatusMeta = {
    unverified: { tone: 'neutral', label: '未检测' },
    verified: { tone: 'success', label: '已生效' },
    invalid: { tone: 'destructive', label: '解析异常' },
} as const;

export const deployStatusMeta: Record<
    WebsiteItem['deploy_status'],
    { tone: StatusTone; label: string }
> = {
    no_nodes: { tone: 'neutral', label: '无可用节点' },
    pending: { tone: 'info', label: '待下发' },
    partial: { tone: 'warning', label: '部分生效' },
    applied: { tone: 'success', label: '已生效' },
};

export function certificateDomainCoversHostname(certificateDomain: string, hostname: string) {
    const normalizedCertificateDomain = certificateDomain.toLowerCase();
    const normalizedHostname = hostname.toLowerCase();
    if (normalizedCertificateDomain === normalizedHostname) return true;
    if (!normalizedCertificateDomain.startsWith('*.')) return false;

    const suffix = normalizedCertificateDomain.slice(2);
    return (
        normalizedHostname.endsWith(`.${suffix}`) &&
        normalizedHostname.split('.').length === suffix.split('.').length + 1
    );
}

export function websiteResolution(item: Pick<WebsiteItem, 'config' | 'runtime'>) {
    const total = item.config.domains.length;
    const verified = item.runtime.domain_states.filter(
        (state) => state.resolution_status === 'verified'
    ).length;
    const invalid = item.runtime.domain_states.some(
        (state) => state.resolution_status === 'invalid'
    );
    if (total > 0 && verified === total) return { tone: 'success', label: '全部生效' } as const;
    if (verified > 0) return { tone: 'warning', label: `${verified}/${total} 生效` } as const;
    if (invalid) return { tone: 'destructive', label: '未生效' } as const;
    return { tone: 'neutral', label: '待检测' } as const;
}

export function currentWebsiteInput(website: WebsiteItem): WebsiteSaveInput {
    return {
        status: website.status,
        config: website.config,
    };
}

export function websiteSaveTarget(website: WebsiteItem) {
    return {
        id: website.id,
        revision: website.revision,
        cluster_id: website.cluster_id,
    };
}

export function createWebsiteInput(data: WebsiteCreateValues, hostname: string): WebsiteSaveInput {
    const httpsEnabled = data.https_enabled;
    return {
        status: 'enabled',
        config: {
            name: data.name.trim(),
            domains: [{ id: createUuid(), hostname, dns_mode: data.dns_mode }],
            origins: [
                {
                    id: createUuid(),
                    group: 'default',
                    protocol: data.origin_protocol,
                    host: data.origin_host.toLowerCase(),
                    port: data.origin_port,
                    role: 'primary',
                    weight: 100,
                    status: 'enabled',
                },
            ],
            default_origin_group: 'default',
            origin_host_header: data.origin_host_header.trim() || '$host',
            origin_connect_timeout_seconds: 10,
            origin_read_timeout_seconds: 30,
            pass_client_ip: true,
            health_check_enabled: true,
            health_check_path: '/',
            health_check_interval_seconds: 10,
            health_check_timeout_seconds: 3,
            health_check_expected_status: 200,
            healthy_threshold: 2,
            unhealthy_threshold: 3,
            access_log_enabled: true,
            access_log_request_headers: false,
            access_log_request_body: false,
            access_log_response_headers: false,
            access_log_query_params: false,
            access_log_cookies: false,
            access_log_referer: false,
            access_log_user_agent: false,
            access_log_status_code_ranges: ['1xx', '2xx', '3xx', '4xx', '5xx'],
            access_log_client_abort: false,
            https_enabled: httpsEnabled,
            certificate_ids: httpsEnabled ? data.certificate_ids : [],
            minimum_tls_version: data.minimum_tls_version,
            force_https: httpsEnabled && data.force_https,
            http2_enabled: data.http2_enabled,
            hsts_enabled: httpsEnabled && data.hsts_enabled,
            response_compression_enabled: data.response_compression_enabled,
            response_compression_min_bytes: data.response_compression_min_bytes,
            response_compression_max_bytes: data.response_compression_max_bytes,
            response_compression_algorithms: data.response_compression_algorithms,
            response_compression_mime_types: data.response_compression_mime_types,
            response_compression_extensions: data.response_compression_extensions,
            response_compression_excluded_extensions: data.response_compression_excluded_extensions,
            route_rules: [],
        },
    };
}

export const defaultResponseCompressionMimeTypes = [
    'text/*',
    'application/javascript',
    'application/x-javascript',
    'application/ecmascript',
    'application/json',
    'application/ld+json',
    'application/manifest+json',
    'application/vnd.api+json',
    'application/geo+json',
    'application/activity+json',
    'application/feed+json',
    'application/hal+json',
    'application/json-patch+json',
    'application/xml',
    'application/xhtml+xml',
    'application/rss+xml',
    'application/atom+xml',
    'application/mathml+xml',
    'application/xslt+xml',
    'application/graphql',
    'application/graphql-response+json',
    'application/wasm',
    'image/svg+xml',
    'font/ttf',
    'font/otf',
    'application/vnd.ms-fontobject',
];

export function createWebsiteDefaults(): WebsiteCreateValues {
    return {
        name: '',
        cluster_id: '',
        hostname: '',
        dns_mode: 'managed',
        managed_dns_zone_id: undefined,
        subdomain_prefix: '',
        origin_protocol: 'http',
        origin_host: '',
        origin_port: 80,
        origin_host_header: '',
        https_enabled: false,
        certificate_ids: [],
        minimum_tls_version: '1.2',
        force_https: true,
        http2_enabled: true,
        hsts_enabled: false,
        response_compression_enabled: true,
        response_compression_min_bytes: 1024,
        response_compression_max_bytes: 32 * 1024 * 1024,
        response_compression_algorithms: ['zstd', 'br', 'gzip'],
        response_compression_mime_types: [...defaultResponseCompressionMimeTypes],
        response_compression_extensions: [
            '.js',
            '.json',
            '.html',
            '.htm',
            '.xml',
            '.css',
            '.woff2',
            '.txt',
        ],
        response_compression_excluded_extensions: ['.apk', '.ipa'],
    };
}
