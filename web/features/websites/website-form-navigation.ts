import type { FieldErrors } from 'react-hook-form'
import type { routeTabs } from './route-tabs'
import type { WebsiteFormValues } from './website-form'

type WebsiteSection =
  | 'basic'
  | 'domains'
  | 'origins'
  | 'origin-settings'
  | 'health'
  | 'https'
  | 'logs'
  | 'compression'
  | keyof typeof routeTabs

// Adding a form field requires choosing its owning section at compile time.
const fieldSections = {
  cluster_id: 'basic',
  status: 'basic',
  name: 'basic',
  domains: 'domains',
  origins: 'origins',
  default_origin_group: 'basic',
  origin_host_header: 'basic',
  origin_connect_timeout_seconds: 'origin-settings',
  origin_read_timeout_seconds: 'origin-settings',
  pass_client_ip: 'origin-settings',
  health_check_enabled: 'health',
  health_check_path: 'health',
  health_check_interval_seconds: 'health',
  health_check_timeout_seconds: 'health',
  health_check_expected_status: 'health',
  healthy_threshold: 'health',
  unhealthy_threshold: 'health',
  access_log_enabled: 'logs',
  access_log_request_headers: 'logs',
  access_log_request_body: 'logs',
  access_log_response_headers: 'logs',
  access_log_query_params: 'logs',
  access_log_cookies: 'logs',
  access_log_referer: 'logs',
  access_log_user_agent: 'logs',
  access_log_status_code_ranges: 'logs',
  access_log_client_abort: 'logs',
  https_enabled: 'https',
  certificate_ids: 'https',
  minimum_tls_version: 'https',
  force_https: 'https',
  http2_enabled: 'https',
  hsts_enabled: 'https',
  response_compression_enabled: 'compression',
  response_compression_min_bytes: 'compression',
  response_compression_max_bytes: 'compression',
  response_compression_algorithms: 'compression',
  response_compression_mime_types: 'compression',
  response_compression_extensions: 'compression',
  response_compression_excluded_extensions: 'compression',
  route_rules: 'proxy',
} satisfies Record<keyof WebsiteFormValues, WebsiteSection>

export function websiteErrorTarget(
  errors: FieldErrors<WebsiteFormValues>,
  rules: WebsiteFormValues['route_rules']
): { section: WebsiteSection; routeIndex: number | null } {
  const field = (Object.keys(errors) as Array<keyof typeof errors>).find(
    (key) => errors[key] !== undefined
  )
  if (field === 'route_rules') {
    const first = Object.keys(errors.route_rules ?? {})
      .filter((key) => /^\d+$/.test(key))
      .map(Number)
      .filter(
        (index) => index < rules.length && errors.route_rules?.[index] != null
      )
      .sort((left, right) => left - right)[0]
    // Array-level errors have no row to expand; show the first existing phase.
    return {
      section: rules[first ?? 0]?.action ?? 'proxy',
      routeIndex: first ?? null,
    }
  }
  return {
    section:
      field && Object.hasOwn(fieldSections, field)
        ? fieldSections[field as keyof typeof fieldSections]
        : 'basic',
    routeIndex: null,
  }
}
