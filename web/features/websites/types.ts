import type { Revisioned } from '@/lib/types'

export type WebsiteDomain = {
  id: string
  hostname: string
  dns_mode: 'managed' | 'external'
}

export type WebsiteOrigin = {
  id: string
  group: string
  protocol: 'http' | 'https'
  host: string
  port: number
  role: 'primary' | 'backup'
  weight: number
  status: 'enabled' | 'disabled'
}

export type WebsiteConfig = {
  name?: string
  domains: WebsiteDomain[]
  origins: WebsiteOrigin[]
  default_origin_group: string
  origin_host_header: string
  origin_connect_timeout_seconds: number
  origin_read_timeout_seconds: number
  pass_client_ip: boolean
  health_check_enabled: boolean
  health_check_path: string
  health_check_interval_seconds: number
  health_check_timeout_seconds: number
  health_check_expected_status: number
  healthy_threshold: number
  unhealthy_threshold: number
  access_log_enabled: boolean
  access_log_request_headers: boolean
  access_log_request_body: boolean
  access_log_response_headers: boolean
  access_log_query_params: boolean
  access_log_cookies: boolean
  access_log_referer: boolean
  access_log_user_agent: boolean
  access_log_status_code_ranges: string[]
  access_log_client_abort: boolean
  https_enabled: boolean
  certificate_ids: string[]
  minimum_tls_version: '1.2' | '1.3'
  force_https: boolean
  http2_enabled: boolean
  hsts_enabled: boolean
  response_compression_enabled: boolean
  response_compression_min_bytes: number
  response_compression_max_bytes: number
  response_compression_algorithms: string[]
  response_compression_mime_types: string[]
  response_compression_extensions: string[]
  response_compression_excluded_extensions: string[]
  route_rules: Array<Record<string, unknown>>
}

export type Website = Revisioned & {
  cluster_id: string
  cluster_name: string
  access_domain: string
  status: string
  config: WebsiteConfig
  certificates: Array<{ id: string; domains: string[]; usable: boolean }>
  runtime: {
    domain_states: Array<{
      id: string
      access_protocol: string
      resolution_status: string
      last_verified_at?: string
      last_error?: string
    }>
    origin_states: Array<{
      node_id: string
      node_name: string
      origin_id: string
      status: string
      checked_at_unix_millis: number
      latency_millis: number
      last_error?: string
    }>
    deploy_status: string
    target_node_count: number
    synced_node_count: number
  }
}

export type WebsiteDashboardSeriesPoint = {
  timestamp: string
  request_count: number
  response_bytes: number
  bandwidth_bps: number
}

export type WebsiteDashboardRanking = {
  label: string
  request_count: number
  response_bytes: number
}

export type WebsiteDashboard = {
  summary: {
    previous_month_peak_bps: number
    current_month_peak_bps: number
    today_peak_bps: number
    current_bandwidth_bps: number
    today_unique_ips: number
    today_response_bytes: number
  }
  hourly: WebsiteDashboardSeriesPoint[]
  daily: WebsiteDashboardSeriesPoint[]
  status_codes: WebsiteDashboardRanking[]
  methods: WebsiteDashboardRanking[]
  countries: WebsiteDashboardRanking[]
  hosts: WebsiteDashboardRanking[]
  referers: WebsiteDashboardRanking[]
  paths: WebsiteDashboardRanking[]
  client_ips_by_bytes: WebsiteDashboardRanking[]
  client_ips_by_requests: WebsiteDashboardRanking[]
}
