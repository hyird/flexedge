import type { Website, WebsiteConfig } from './types'
import {
  defaultWebsiteConfig,
  parseRouteHeaders,
  routeRuleToForm,
  type WebsiteFormValues,
} from './website-form'

export function websiteToFormValues(
  website?: Pick<Website, 'cluster_id' | 'status' | 'config'>
): WebsiteFormValues {
  const config = structuredClone(website?.config ?? defaultWebsiteConfig())
  return {
    cluster_id: website?.cluster_id ?? '',
    status: (website?.status as WebsiteFormValues['status']) ?? 'enabled',
    name: config.name ?? '',
    domains:
      config.domains.length > 0
        ? config.domains
        : [
            {
              id: crypto.randomUUID(),
              hostname: '',
              dns_mode: 'managed' as const,
            },
          ],
    origins:
      config.origins.length > 0
        ? config.origins
        : [
            {
              id: crypto.randomUUID(),
              group: 'default',
              protocol: 'http' as const,
              host: '',
              port: 80,
              role: 'primary' as const,
              weight: 100,
              status: 'enabled' as const,
            },
          ],
    default_origin_group: config.default_origin_group,
    origin_host_header: config.origin_host_header,
    origin_connect_timeout_seconds: config.origin_connect_timeout_seconds,
    origin_read_timeout_seconds: config.origin_read_timeout_seconds,
    pass_client_ip: config.pass_client_ip,
    health_check_enabled: config.health_check_enabled,
    health_check_path: config.health_check_path,
    health_check_interval_seconds: config.health_check_interval_seconds,
    health_check_timeout_seconds: config.health_check_timeout_seconds,
    health_check_expected_status: config.health_check_expected_status,
    healthy_threshold: config.healthy_threshold,
    unhealthy_threshold: config.unhealthy_threshold,
    access_log_enabled: config.access_log_enabled,
    access_log_request_headers: config.access_log_request_headers,
    access_log_request_body: config.access_log_request_body,
    access_log_response_headers: config.access_log_response_headers,
    access_log_query_params: config.access_log_query_params,
    access_log_cookies: config.access_log_cookies,
    access_log_referer: config.access_log_referer,
    access_log_user_agent: config.access_log_user_agent,
    access_log_status_code_ranges: config.access_log_status_code_ranges.filter(
      (
        range
      ): range is WebsiteFormValues['access_log_status_code_ranges'][number] =>
        ['1xx', '2xx', '3xx', '4xx', '5xx'].includes(range)
    ),
    access_log_client_abort: config.access_log_client_abort,
    https_enabled: config.https_enabled,
    certificate_ids: config.certificate_ids,
    minimum_tls_version: config.minimum_tls_version,
    force_https: config.force_https,
    http2_enabled: config.http2_enabled,
    hsts_enabled: config.hsts_enabled,
    response_compression_enabled: config.response_compression_enabled,
    response_compression_min_bytes: config.response_compression_min_bytes,
    response_compression_max_bytes: config.response_compression_max_bytes,
    response_compression_algorithms:
      config.response_compression_algorithms.filter(
        (
          algorithm
        ): algorithm is WebsiteFormValues['response_compression_algorithms'][number] =>
          ['br', 'zstd', 'gzip'].includes(algorithm)
      ),
    response_compression_mime_types: config.response_compression_mime_types,
    response_compression_extensions: config.response_compression_extensions,
    response_compression_excluded_extensions:
      config.response_compression_excluded_extensions,
    route_rules: config.route_rules.map(routeRuleToForm),
  }
}

export function websiteFormToConfig(values: WebsiteFormValues): WebsiteConfig {
  const {
    cluster_id: _clusterId,
    status: _status,
    route_rules,
    ...config
  } = values
  return {
    ...structuredClone(config),
    route_rules: route_rules.map(
      ({ request_headers_text, response_headers_text, ...rule }) => ({
        ...structuredClone(rule),
        request_headers: parseRouteHeaders(request_headers_text),
        response_headers: parseRouteHeaders(response_headers_text),
      })
    ),
  }
}
