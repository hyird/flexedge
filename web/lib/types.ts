export type AuthUser = {
  id: string
  username: string
  nickname?: string
  status: string
}

export type Revisioned = {
  id: string
  revision: number
  created_at: string
  updated_at: string
}

export type PageData<T> = {
  list: T[]
  total: number
  page: number
  page_size: number
  total_pages: number
}

export type OverviewTask = {
  id: string
  kind: string
  resource_type: string
  resource_id: string
  resource_name: string
  operation: string
  status: string
  last_error?: string
  updated_at: string
}

export type OverviewData = {
  resources: {
    website_count: number
    domain_count: number
    certificate_count: number
    cluster_count: number
  }
  issues: {
    dns_zone_issue_count: number
    certificate_expiring_count: number
    certificate_failed_count: number
    active_task_count: number
    failed_task_count: number
  }
  recent_tasks: OverviewTask[]
}

export type Task = {
  id: string
  sequence: number
  kind: string
  resource_type: string
  resource_id: string
  resource_name: string
  operation: string
  status: string
  version: number
  processed_version: number
  count_fails: number
  next_attempt_at: string
  lease_until?: string
  error?: string
  completed_at?: string
  created_at: string
  updated_at: string
}

export type TaskPage = PageData<Task> & {
  summary: {
    pending: number
    running: number
    retry: number
    completed: number
  }
}

export type DnsProvider = Revisioned & {
  name: string
  account_id: string
  provider: string
  token_hint: string
  status: string
  zone_count: number
  last_verified_at?: string
  last_error?: string
}

export type CertificateProvider = Revisioned & {
  provider: string
  credential_mode: string
  account_email?: string
  access_key_hint?: string
  status: string
  last_verified_at?: string
  last_error?: string
}

export type DnsLine = {
  code: string
  name: string
  display_name: string
  status: string
}

export type DnsRecord = {
  id: string
  type: 'A' | 'AAAA' | 'CNAME' | 'TXT' | 'MX'
  name: string
  content: string
  ttl: number
  priority?: number
  proxied: boolean
  line_code: string
}

export type DnsZone = Revisioned & {
  dns_provider_id: string
  dns_provider: string
  dns_provider_name: string
  domain: string
  sync_status: string
  desired_revision: number
  synced_revision: number
  website_count: number
  config: { records: DnsRecord[] }
  runtime: {
    records_imported: boolean
    lines_synced_at?: string
    lines: DnsLine[]
    projected_records: DnsRecord[]
    record_states: Array<{
      id: string
      sync_status: string
      synced_revision: number
      last_error?: string
    }>
    conflicts: Array<{
      id: string
      type: string
      name: string
      line_code: string
      local_content: string
      remote_content: string
    }>
  }
  last_synced_at?: string
  last_error?: string
}

export type DnsZoneOption = {
  id: string
  domain: string
  dns_provider: string
  dns_provider_name: string
  sync_status: string
  available: boolean
}

export type Cluster = Revisioned & {
  name: string
  dns_zone_id: string
  dns_zone_domain: string
  dns_provider_name: string
  hostname_prefix: string
  access_domain: string
  node_count: number
  online_node_count: number
  status: string
}

export type NodeEndpoint = {
  id: string
  ip_address: string
  line_code: string
}

export type Node = Revisioned & {
  cluster_id: string
  cluster_name: string
  name: string
  status: string
  node_spec_revision: number
  config: { endpoints: NodeEndpoint[] }
  runtime: {
    registration_status: string
    connection_status: string
    last_heartbeat_at?: string
    applied_node_spec_revision: number
    active_release_id?: string
    active_manifest_digest?: string
    agent_version?: string
    cpu_usage?: number
    memory_usage?: number
    traffic_out_bps?: number
    connection_count?: number
    load_1m?: number
    queued_log_events?: number
    dropped_log_events?: number
    health?: string
    last_error?: string
  }
}

export type Certificate = Revisioned & {
  domains: string[]
  issuer: string
  certificate_provider_id: string
  certificate_provider: string
  status: string
  usable: boolean
  config: { auto_renew: boolean }
  dns_zone_id: string
  dns_zone_domain: string
  not_before?: string
  expires_at?: string
  remaining_days?: number
  last_error?: string
  serial_number?: string
  fingerprint_sha256?: string
  last_issued_at?: string
  sync_status?: string
  sync_count_fails?: number
  website_count: number
}

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
