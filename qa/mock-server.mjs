// Local-only browser QA. Never imported by the production app.
import { randomUUID } from 'node:crypto'
import { readFile } from 'node:fs/promises'
import { createServer } from 'node:http'
import { resolve } from 'node:path'
import { fileURLToPath } from 'node:url'

const root = fileURLToPath(new URL('.', import.meta.url))
const policyFixture = JSON.parse(
  await readFile(resolve(root, 'fixtures/cache-policies.json'), 'utf8')
)
const jobsFixture = JSON.parse(
  await readFile(resolve(root, 'fixtures/cache-jobs.json'), 'utf8')
)
const uuid = (number) =>
  `00000000-0000-4000-8000-${String(number).padStart(12, '0')}`
const initialTime = '2026-10-01T01:30:00.000Z'
const timestamps = { created_at: initialTime, updated_at: initialTime }
let tick = 0
const nextTime = () =>
  new Date(Date.parse(initialTime) + ++tick * 1000).toISOString()
const websiteId = jobsFixture.data.list[0].website_id
const zoneId = uuid(981),
  clusterId = uuid(951),
  nodeId = uuid(971),
  certificateId = uuid(991),
  dnsProviderId = uuid(801),
  certificateProviderId = uuid(811)
const lines = [
  {
    code: 'default',
    name: '默认',
    display_name: '默认线路',
    status: 'enabled',
  },
  {
    code: 'telecom',
    name: '电信',
    display_name: '中国_电信',
    status: 'enabled',
  },
  {
    code: 'unicom',
    name: '联通',
    display_name: '中国_联通',
    status: 'enabled',
  },
  { code: 'oversea', name: '境外', display_name: '境外', status: 'enabled' },
]
const cacheConfig = {
  memory_bytes: 67108864,
  disk_bytes: 1073741824,
  max_object_bytes: 268435456,
  directory: 'cache',
}
const cacheStatistics = {
  memory_hits: 1284,
  disk_hits: 462,
  misses: 93,
  bypasses: 17,
  stale_hits: 8,
  revalidations: 12,
  origin_bytes: 18350080,
  cache_bytes: 150994944,
  coalesced: 28,
  active_waiters: 2,
  memory_bytes: 9437184,
  disk_bytes: 71303168,
  entries: 384,
  writes: 402,
  write_errors: 0,
  evictions: 19,
}
const websiteConfig = {
  name: 'QA 网站',
  cache_enabled: true,
  cache_policy_id: policyFixture.data.list[0].id,
  domains: [
    { id: uuid(961), hostname: 'qa.example.test', dns_mode: 'managed' },
  ],
  origins: [
    {
      id: uuid(962),
      group: 'default',
      protocol: 'https',
      host: 'origin.qa.test',
      port: 443,
      role: 'primary',
      weight: 100,
      status: 'enabled',
    },
  ],
  default_origin_group: 'default',
  origin_host_header: '$host',
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
  access_log_query_params: true,
  access_log_cookies: false,
  access_log_referer: true,
  access_log_user_agent: true,
  access_log_status_code_ranges: ['2xx', '4xx', '5xx'],
  access_log_client_abort: true,
  https_enabled: true,
  certificate_ids: [certificateId],
  minimum_tls_version: '1.2',
  force_https: false,
  http2_enabled: true,
  hsts_enabled: false,
  response_compression_enabled: true,
  response_compression_min_bytes: 1024,
  response_compression_max_bytes: 0,
  response_compression_algorithms: ['br', 'gzip'],
  response_compression_mime_types: ['text/*', 'application/json'],
  response_compression_extensions: [],
  response_compression_excluded_extensions: [],
  route_rules: [],
}
let db = {
  dnsProviders: [
    {
      id: dnsProviderId,
      revision: 3,
      name: 'Cloudflare 生产账号',
      provider: 'cloudflare',
      account_id: 'qa-account-01',
      token_hint: 'qa…0001',
      status: 'verified',
      zone_count: 1,
      last_verified_at: initialTime,
      ...timestamps,
    },
    {
      id: uuid(802),
      revision: 1,
      name: '阿里云备用账号',
      provider: 'aliyun',
      account_id: 'qa-account-02',
      token_hint: 'qa…0002',
      status: 'unverified',
      zone_count: 1,
      ...timestamps,
    },
  ],
  certificateProviders: [
    {
      id: certificateProviderId,
      revision: 2,
      provider: 'letsencrypt',
      credential_mode: 'email',
      account_email: 'qa@example.test',
      status: 'verified',
      last_verified_at: initialTime,
      ...timestamps,
    },
    {
      id: uuid(812),
      revision: 1,
      provider: 'zerossl',
      credential_mode: 'access_key',
      access_key_hint: 'qa…0003',
      status: 'unverified',
      ...timestamps,
    },
  ],
  zones: [
    {
      id: zoneId,
      revision: 4,
      dns_provider_id: dnsProviderId,
      dns_provider: 'cloudflare',
      dns_provider_name: 'Cloudflare 生产账号',
      domain: 'example.test',
      sync_status: 'synced',
      desired_revision: 4,
      synced_revision: 4,
      website_count: 1,
      config: {
        records: [
          {
            id: uuid(982),
            type: 'A',
            name: 'qa',
            content: '192.0.2.10',
            ttl: 300,
            proxied: false,
            line_code: 'default',
          },
        ],
      },
      runtime: {
        records_imported: true,
        lines_synced_at: initialTime,
        lines: structuredClone(lines),
        projected_records: [],
        record_states: [
          { id: uuid(982), sync_status: 'synced', synced_revision: 4 },
        ],
        conflicts: [],
      },
      last_synced_at: initialTime,
      ...timestamps,
    },
    {
      id: uuid(983),
      revision: 2,
      dns_provider_id: uuid(802),
      dns_provider: 'aliyun',
      dns_provider_name: '阿里云备用账号',
      domain: 'backup.test',
      sync_status: 'failed',
      desired_revision: 2,
      synced_revision: 1,
      website_count: 0,
      config: { records: [] },
      runtime: {
        records_imported: true,
        lines: structuredClone(lines),
        projected_records: [],
        record_states: [],
        conflicts: [],
      },
      last_error: 'QA 场景：供应商令牌等待验证',
      ...timestamps,
    },
  ],
  clusters: [
    {
      id: clusterId,
      revision: 2,
      name: 'QA Cluster',
      dns_zone_id: zoneId,
      dns_zone_domain: 'example.test',
      dns_provider_name: 'Cloudflare 生产账号',
      hostname_prefix: 'edge',
      access_domain: 'edge.example.test',
      node_count: 3,
      online_node_count: 2,
      status: 'enabled',
      ...timestamps,
    },
    {
      id: uuid(952),
      revision: 1,
      name: '备用集群',
      dns_zone_id: zoneId,
      dns_zone_domain: 'example.test',
      dns_provider_name: 'Cloudflare 生产账号',
      hostname_prefix: 'backup',
      access_domain: 'backup.example.test',
      node_count: 0,
      online_node_count: 0,
      status: 'disabled',
      ...timestamps,
    },
  ],
  nodes: [
    {
      id: nodeId,
      revision: 2,
      cluster_id: clusterId,
      cluster_name: 'QA Cluster',
      name: 'QA Edge 01',
      status: 'enabled',
      node_spec_revision: 2,
      config: {
        endpoints: [
          { id: uuid(972), ip_address: '192.0.2.10', line_code: 'default' },
        ],
        cache: structuredClone(cacheConfig),
      },
      runtime: {
        registration_status: 'registered',
        connection_status: 'online',
        last_heartbeat_at: initialTime,
        applied_node_spec_revision: 2,
        active_release_id: uuid(701),
        active_manifest_digest: 'a'.repeat(64),
        agent_version: '1.8.0',
        cpu_usage: 12.4,
        memory_usage: 28.6,
        traffic_out_bps: 3145728,
        connection_count: 426,
        load_1m: 0.24,
        queued_log_events: 2,
        dropped_log_events: 0,
        health: 'healthy',
        cache_statistics: structuredClone(cacheStatistics),
      },
      ...timestamps,
    },
    {
      id: uuid(973),
      revision: 3,
      cluster_id: clusterId,
      cluster_name: 'QA Cluster',
      name: 'QA Edge 02',
      status: 'enabled',
      node_spec_revision: 3,
      config: {
        endpoints: [
          { id: uuid(974), ip_address: '2001:db8::20', line_code: 'telecom' },
        ],
        cache: structuredClone(cacheConfig),
      },
      runtime: {
        registration_status: 'registered',
        connection_status: 'online',
        last_heartbeat_at: initialTime,
        applied_node_spec_revision: 3,
        agent_version: '1.8.0',
        cpu_usage: 8.2,
        memory_usage: 19.5,
        traffic_out_bps: 1048576,
        connection_count: 187,
        load_1m: 0.12,
        cache_statistics: structuredClone(cacheStatistics),
      },
      ...timestamps,
    },
    {
      id: uuid(975),
      revision: 1,
      cluster_id: clusterId,
      cluster_name: 'QA Cluster',
      name: 'QA Edge 03',
      status: 'disabled',
      node_spec_revision: 1,
      config: {
        endpoints: [
          { id: uuid(976), ip_address: '192.0.2.30', line_code: 'oversea' },
        ],
        cache: structuredClone(cacheConfig),
      },
      runtime: {
        registration_status: 'pending',
        connection_status: 'unregistered',
        applied_node_spec_revision: 0,
      },
      ...timestamps,
    },
  ],
  certificates: [
    {
      id: certificateId,
      revision: 2,
      domains: ['qa.example.test'],
      issuer: "Let's Encrypt",
      certificate_provider_id: certificateProviderId,
      certificate_provider: 'letsencrypt',
      status: 'valid',
      usable: true,
      config: { auto_renew: true },
      dns_zone_id: zoneId,
      dns_zone_domain: 'example.test',
      not_before: '2026-10-01T00:00:00Z',
      expires_at: '2026-12-30T00:00:00Z',
      remaining_days: 90,
      serial_number: 'QA000001',
      fingerprint_sha256: 'ab'.repeat(32),
      last_issued_at: initialTime,
      sync_status: 'completed',
      sync_count_fails: 0,
      website_count: 1,
      ...timestamps,
    },
    {
      id: uuid(992),
      revision: 1,
      domains: ['static.example.test'],
      issuer: "Let's Encrypt",
      certificate_provider_id: certificateProviderId,
      certificate_provider: 'letsencrypt',
      status: 'issuing',
      usable: false,
      config: { auto_renew: true },
      dns_zone_id: zoneId,
      dns_zone_domain: 'example.test',
      sync_status: 'running',
      sync_count_fails: 0,
      website_count: 0,
      ...timestamps,
    },
  ],
  websites: [
    {
      id: websiteId,
      revision: 4,
      cluster_id: clusterId,
      cluster_name: 'QA Cluster',
      access_domain: 'edge.example.test',
      status: 'enabled',
      config: structuredClone(websiteConfig),
      certificates: [
        { id: certificateId, domains: ['qa.example.test'], usable: true },
      ],
      runtime: {
        domain_states: [
          {
            id: uuid(961),
            access_protocol: 'https',
            resolution_status: 'resolved',
            last_verified_at: initialTime,
          },
        ],
        origin_sources: [
          { node_id: nodeId, node_revision: 2, reported_at: initialTime },
        ],
        origin_states: [
          {
            node_id: nodeId,
            node_name: 'QA Edge 01',
            origin_id: uuid(962),
            status: 'healthy',
            checked_at_unix_millis: Date.parse(initialTime),
            latency_millis: 23,
          },
        ],
        deploy_status: 'synced',
        target_node_count: 2,
        synced_node_count: 2,
      },
      ...timestamps,
    },
    {
      id: uuid(922),
      revision: 1,
      cluster_id: clusterId,
      cluster_name: 'QA Cluster',
      access_domain: 'edge.example.test',
      status: 'disabled',
      config: {
        ...structuredClone(websiteConfig),
        name: '静态资源站点',
        domains: [
          {
            id: uuid(963),
            hostname: 'static.example.test',
            dns_mode: 'external',
          },
        ],
        cache_enabled: false,
        cache_policy_id: '',
        certificate_ids: [],
        https_enabled: false,
      },
      certificates: [],
      runtime: {
        domain_states: [
          {
            id: uuid(963),
            access_protocol: 'http',
            resolution_status: 'pending',
          },
        ],
        origin_sources: [],
        origin_states: [],
        deploy_status: 'pending',
        target_node_count: 2,
        synced_node_count: 0,
      },
      ...timestamps,
    },
  ],
  policies: [
    structuredClone(policyFixture.data.list[0]),
    {
      ...structuredClone(policyFixture.data.list[0]),
      id: uuid(902),
      name: 'QA API 绕过策略',
      description: '仅供本地视觉回归，适合动态接口',
      status: 'disabled',
      revision: 1,
      website_count: 0,
      websites: [],
      rules: [
        {
          id: uuid(912),
          name: '全部绕过',
          status: 'enabled',
          match_type: 'all',
          patterns: [],
          action: 'bypass',
          ttl_seconds: 0,
          query_mode: 'include',
          status_codes: [200],
          min_object_bytes: 0,
          max_object_bytes: 0,
          stale_if_error_seconds: 0,
          ignore_origin_cache_control: false,
          range_enabled: false,
        },
      ],
      ...timestamps,
    },
  ],
  jobs: structuredClone(jobsFixture.data.list),
  tasks: [
    {
      id: 'certificate:issue:qa',
      resource_type: 'certificate',
      resource_id: uuid(992),
      name: 'static.example.test',
      operation: 'issue',
      version: 1,
      status: 'running',
      error: '',
      failures: 0,
      updated_at: initialTime,
      next_attempt_at: '',
    },
    {
      id: 'dns-zone:sync:qa',
      resource_type: 'dns_zone',
      resource_id: uuid(983),
      name: 'backup.test',
      operation: 'sync',
      version: 2,
      status: 'retrying',
      error: 'QA 场景：供应商令牌等待验证',
      failures: 2,
      updated_at: initialTime,
      next_attempt_at: '2026-10-01T01:35:00Z',
    },
    {
      id: 'website:deploy:qa',
      resource_type: 'website',
      resource_id: websiteId,
      name: 'QA 网站',
      operation: 'deploy',
      version: 4,
      status: 'completed',
      error: '',
      failures: 0,
      updated_at: initialTime,
      next_attempt_at: '',
    },
  ],
  nodeLogs: {
    [nodeId]: [
      {
        id: uuid(601),
        occurred_at: initialTime,
        level: 'info',
        category: 'control',
        message: '节点已连接，配置 revision 2 已生效。',
      },
      {
        id: uuid(602),
        occurred_at: '2026-10-01T01:29:00Z',
        level: 'warn',
        category: 'origin',
        message: '回源延迟超过阈值，后续请求保持健康检查。',
      },
    ],
  },
  accessLogs: {
    [websiteId]: [
      {
        id: uuid(611),
        occurred_at: initialTime,
        node_id: nodeId,
        node_name: 'QA Edge 01',
        client_ip: '198.51.100.20',
        client_ip_location: '台湾',
        protocol: 'HTTP/2',
        method: 'GET',
        host: 'qa.example.test',
        target: '/assets/app.js',
        status_code: 200,
        request_bytes: 384,
        response_bytes: 16384,
        duration_ms: 12,
        user_agent: 'QA Browser',
        referer: 'https://qa.example.test/',
        request_body_truncated: false,
        cache_status: 'hit',
        cache_layer: 'memory',
        origin_bytes: 0,
        cache_bytes: 16384,
      },
      {
        id: uuid(612),
        occurred_at: '2026-10-01T01:29:00Z',
        node_id: nodeId,
        node_name: 'QA Edge 01',
        client_ip: '198.51.100.21',
        protocol: 'HTTP/1.1',
        method: 'GET',
        host: 'qa.example.test',
        target: '/missing',
        status_code: 404,
        request_bytes: 128,
        response_bytes: 512,
        duration_ms: 29,
        request_body_truncated: false,
        cache_status: 'miss',
        cache_layer: 'none',
        origin_bytes: 512,
        cache_bytes: 0,
      },
    ],
  },
}
for (const job of db.jobs) {
  job.error ??= ''
  for (const node of job.nodes) {
    node.node_id = nodeId
    node.command_id ??= uuid(631)
    node.error ??= ''
    for (const result of node.results) result.error ??= ''
  }
}
const seed = structuredClone(db)
const credentials = new Map(
  db.nodes.map((node) => [node.id, `qa-only-secret-${node.id}`])
)
const user = {
  id: uuid(101),
  username: 'admin',
  nickname: 'QA 管理员',
  status: 'active',
}
const activeStreams = new Map(),
  requestCounts = new Map()
let streamSequence = 0,
  emittedEvents = 0,
  totalRequests = 0
const envelope = (data, message = 'ok') => ({
  code: 0,
  message,
  ...(data !== undefined ? { data } : {}),
})
const errorEnvelope = (message, code = 40001) => ({ code, message })
function fail(message, status = 400, code = 40001) {
  throw Object.assign(new Error(message), { status, code })
}
function find(list, id) {
  const item = list.find((value) => value.id === id)
  if (!item) fail('资源不存在', 404)
  return item
}
function requireRevision(req, item) {
  const header = req.headers['if-match']
  if (!header) fail('更新资源必须提供 If-Match revision ETag', 428)
  if (!/^"[1-9]\d*"$/.test(header))
    fail('If-Match 必须是带引号的正整数 revision ETag')
  if (Number(header.slice(1, -1)) !== item.revision) {
    const path = new URL(req.url, 'http://127.0.0.1').pathname
    const codes = {
      '/api/nodes': 16505,
      '/api/clusters': 16405,
      '/api/websites': 16614,
      '/api/dns-zones': 16205,
      '/api/providers/dns': 16107,
      '/api/providers/certificate': 16016,
      '/api/certificates': 16016,
      '/api/cache-policies': 17104,
    }
    const code = path.includes('/cache-jobs')
      ? 17202
      : (Object.entries(codes).find(([prefix]) =>
          path.startsWith(prefix)
        )?.[1] ?? 40001)
    fail('配置已被其他请求修改，请刷新后重试', 412, code)
  }
}
function touch(item) {
  item.revision += 1
  item.updated_at = nextTime()
}
function refreshRelations() {
  for (const cluster of db.clusters) {
    const zone = db.zones.find((value) => value.id === cluster.dns_zone_id)
    const nodes = db.nodes.filter((value) => value.cluster_id === cluster.id)
    Object.assign(cluster, {
      dns_zone_domain: zone?.domain ?? '',
      dns_provider_name: zone?.dns_provider_name ?? '',
      access_domain: `${cluster.hostname_prefix}.${zone?.domain ?? ''}`,
      node_count: nodes.length,
      online_node_count: nodes.filter(
        (value) => value.runtime.connection_status === 'online'
      ).length,
    })
  }
  for (const node of db.nodes)
    node.cluster_name =
      db.clusters.find((value) => value.id === node.cluster_id)?.name ?? ''
  for (const website of db.websites) {
    const cluster = db.clusters.find((value) => value.id === website.cluster_id)
    website.cluster_name = cluster?.name ?? ''
    website.access_domain = cluster?.access_domain ?? ''
    website.certificates = db.certificates
      .filter((value) => website.config.certificate_ids.includes(value.id))
      .map(({ id, domains, usable }) => ({ id, domains, usable }))
  }
  for (const provider of db.dnsProviders)
    provider.zone_count = db.zones.filter(
      (value) => value.dns_provider_id === provider.id
    ).length
  for (const zone of db.zones) {
    const provider = db.dnsProviders.find(
      (value) => value.id === zone.dns_provider_id
    )
    zone.dns_provider_name = provider?.name ?? ''
    zone.website_count = db.websites.filter((value) =>
      value.config.domains.some(
        (domain) =>
          domain.hostname === zone.domain ||
          domain.hostname.endsWith(`.${zone.domain}`)
      )
    ).length
  }
  for (const certificate of db.certificates)
    certificate.website_count = db.websites.filter((value) =>
      value.config.certificate_ids.includes(certificate.id)
    ).length
  for (const policy of db.policies) {
    const websites = db.websites.filter(
      (value) => value.config.cache_policy_id === policy.id
    )
    policy.website_count = websites.length
    policy.websites = websites.map((value) => ({
      id: value.id,
      name: value.config.name,
      status: value.status,
    }))
  }
}
function filtered(list, query) {
  const keyword = query.get('keyword')?.toLowerCase()
  return list.filter(
    (item) =>
      (!keyword || JSON.stringify(item).toLowerCase().includes(keyword)) &&
      [
        'status',
        'cluster_id',
        'dns_zone_id',
        'dns_provider_id',
        'registration_status',
        'connection_status',
      ].every(
        (field) =>
          !query.get(field) ||
          (item[field] ?? item.runtime?.[field]) === query.get(field)
      ) &&
      (!query.get('type') || item.resource_type === query.get('type')) &&
      (!query.get('method') || item.method === query.get('method')) &&
      (!query.get('status_class') ||
        `${Math.floor(item.status_code / 100)}xx` === query.get('status_class'))
  )
}
function page(list, query = new URLSearchParams()) {
  const entries = query.get('qa') === 'empty' ? [] : filtered(list, query)
  const size = Math.min(1000, Math.max(1, Number(query.get('page_size')) || 20))
  const current = Math.max(1, Number(query.get('page')) || 1)
  return {
    list: entries.slice((current - 1) * size, current * size),
    total: entries.length,
    page: current,
    page_size: size,
    total_pages: Math.max(1, Math.ceil(entries.length / size)),
  }
}
function zoneOption(zone) {
  return {
    id: zone.id,
    domain: zone.domain,
    dns_provider: zone.dns_provider,
    dns_provider_name: zone.dns_provider_name,
    sync_status: zone.sync_status,
    available: zone.sync_status === 'synced',
  }
}
function overview() {
  return {
    resources: {
      website_count: db.websites.length,
      domain_count: db.zones.length,
      certificate_count: db.certificates.length,
      cluster_count: db.clusters.length,
    },
    issues: {
      dns_zone_issue_count: db.zones.filter(
        (zone) => zone.sync_status === 'failed'
      ).length,
      certificate_expiring_count: db.certificates.filter(
        (certificate) => certificate.remaining_days < 30
      ).length,
      certificate_failed_count: db.certificates.filter(
        (certificate) => certificate.status === 'failed'
      ).length,
      active_marker_count: db.tasks.filter((task) =>
        ['queued', 'running', 'retrying'].includes(task.status)
      ).length,
      retry_marker_count: db.tasks.filter((task) => task.status === 'retrying')
        .length,
    },
    recent_markers: db.tasks.map((task) => ({
      id: task.id,
      resource_type: task.resource_type,
      resource_id: task.resource_id,
      resource_name: task.name,
      operation: task.operation,
      status: task.status,
      ...(task.error ? { last_error: task.error } : {}),
      updated_at: task.updated_at,
    })),
  }
}
function dashboard() {
  const ranking = (label, count = 1284, bytes = 150994944) => ({
    label,
    request_count: count,
    response_bytes: bytes,
  })
  const series = Array.from({ length: 12 }, (_, index) => ({
    timestamp: new Date(
      Date.parse(initialTime) - (11 - index) * 3600000
    ).toISOString(),
    request_count: 200 + index * 47,
    response_bytes: 8388608 + index * 1048576,
    bandwidth_bps: 524288 + index * 131072,
  }))
  return {
    summary: {
      previous_month_peak_bps: 14680064,
      current_month_peak_bps: 18874368,
      today_peak_bps: 12582912,
      current_bandwidth_bps: 3145728,
      today_unique_ips: 286,
      today_response_bytes: 150994944,
    },
    hourly: series,
    daily: series.slice(0, 7),
    status_codes: [ranking('200'), ranking('404', 12, 6144)],
    methods: [ranking('GET'), ranking('POST', 26, 8192)],
    countries: [ranking('台湾'), ranking('中国', 732, 67108864)],
    hosts: [ranking('qa.example.test')],
    referers: [ranking('直接访问')],
    paths: [ranking('/assets/app.js'), ranking('/api/status', 412, 131072)],
    client_ips_by_bytes: [
      {
        ...ranking('198.51.100.20'),
        asn: 'AS64500',
        as_name: 'QA Documentation Network',
      },
    ],
    client_ips_by_requests: [
      {
        ...ranking('198.51.100.21'),
        asn: 'AS64500',
        as_name: 'QA Documentation Network',
      },
    ],
  }
}
function readStream(url) {
  refreshRelations()
  const path = url.pathname,
    query = url.searchParams,
    part = path.split('/').filter(Boolean)
  if (path === '/api/overview/stream') return overview()
  if (path === '/api/providers/dns/stream') return page(db.dnsProviders, query)
  if (path === '/api/providers/dns/options/stream')
    return filtered(db.dnsProviders, query)
  if (
    path === '/api/providers/certificate/stream' ||
    path === '/api/providers/certificate/options/stream'
  )
    return filtered(db.certificateProviders, query)
  if (part[1] === 'providers' && part[2] === 'dns' && part.length === 5)
    return find(db.dnsProviders, part[3])
  if (path === '/api/dns-zones/stream') return page(db.zones, query)
  if (path === '/api/dns-zones/collection/stream')
    return filtered(db.zones, query).map(zoneOption)
  if (path === '/api/dns-zones/options/stream')
    return { list: filtered(db.zones, query).map(zoneOption) }
  if (path === '/api/dns-zones/available/stream')
    return {
      list: [
        { domain: 'new-example.test', status: 'active' },
        { domain: 'unused-example.test', status: 'active' },
      ],
    }
  if (part[1] === 'dns-zones' && part.length === 4)
    return find(db.zones, part[2])
  if (path === '/api/clusters/stream') return page(db.clusters, query)
  if (path === '/api/clusters/options/stream')
    return filtered(db.clusters, query)
  if (path === '/api/nodes/stream') return page(db.nodes, query)
  if (part[1] === 'nodes' && part[3] === 'logs') {
    find(db.nodes, part[2])
    return { list: db.nodeLogs[part[2]] ?? [] }
  }
  if (path === '/api/certificates/stream') return page(db.certificates, query)
  if (path === '/api/certificates/options/stream')
    return filtered(db.certificates, query).filter(
      (value) => query.get('usable') !== 'true' || value.usable
    )
  if (part[1] === 'certificates' && part.length === 4)
    return find(db.certificates, part[2])
  if (path === '/api/cache-policies/stream') return page(db.policies, query)
  if (path === '/api/cache-policies/options/stream')
    return db.policies
      .filter((policy) => policy.status === 'enabled')
      .map(({ id, name, status, revision }) => ({ id, name, status, revision }))
  if (part[1] === 'cache-policies' && part.length === 4)
    return find(db.policies, part[2])
  if (path === '/api/websites/stream') return page(db.websites, query)
  if (part[1] === 'websites') {
    find(db.websites, part[2])
    if (part[3] === 'access-logs')
      return part[4] === 'history'
        ? page(db.accessLogs[part[2]] ?? [], query)
        : { list: db.accessLogs[part[2]] ?? [] }
    if (part[3] === 'dashboard') return dashboard()
    if (part[3] === 'cache-jobs')
      return page(
        db.jobs.filter((job) => job.website_id === part[2]),
        query
      )
    if (part.length === 4) return find(db.websites, part[2])
  }
  if (path === '/api/tasks/stream')
    return {
      ...page(db.tasks, query),
      active: db.tasks.filter((task) =>
        ['queued', 'running', 'retrying'].includes(task.status)
      ).length,
      failed: db.tasks.filter((task) => task.status === 'failed').length,
    }
  if (part[1] === 'tasks') {
    const task = find(db.tasks, decodeURIComponent(part[2]))
    return part[3] === 'history'
      ? {
          list: [
            {
              outcome: task.error ? 'failed' : 'completed',
              error: task.error,
              emitted_at: task.updated_at,
            },
          ],
          truncated: false,
        }
      : task
  }
  fail(`QA mock 未实现流：${path}`, 404)
}
function emit(stream, event, data, { force = false, cursor } = {}) {
  const payload = typeof data === 'string' ? data : JSON.stringify(data)
  const frame = `${cursor ? `id: ${cursor}\n` : ''}event: ${event}\ndata: ${payload}\n\n`
  if (!force && stream.lastFrame === frame) return
  stream.res.write(frame)
  stream.lastFrame = frame
  stream.eventCount += 1
  emittedEvents += 1
  stream.lastEvent = event
}
function emitSnapshot(stream, force = false) {
  try {
    const event = stream.url.pathname.endsWith('/dashboard/stream')
      ? 'dashboard'
      : 'snapshot'
    emit(stream, event, envelope(readStream(stream.url)), { force })
  } catch (error) {
    emit(stream, 'resource-error', errorEnvelope(error.message), { force })
  }
}
function openStream(req, res, url) {
  res.writeHead(200, {
    'Content-Type': 'text/event-stream',
    'Cache-Control': 'no-cache',
    Connection: 'keep-alive',
    'X-QA-Fixture': '1',
  })
  res.flushHeaders()
  const stream = {
    id: ++streamSequence,
    url,
    res,
    eventCount: 0,
    openedAt: new Date().toISOString(),
    lastFrame: '',
    lastEvent: '',
  }
  activeStreams.set(stream.id, stream)
  if (
    url.searchParams.get('qa') === 'error' ||
    url.searchParams.get('keyword') === '__error__'
  )
    emit(stream, 'resource-error', errorEnvelope('QA 场景：模拟资源加载失败'))
  else if (/\/(?:logs|access-logs)\/stream$/.test(url.pathname)) {
    emit(stream, 'ready', '{}')
    try {
      const data = readStream(url)
      const after =
        url.searchParams.get('after') ?? req.headers['last-event-id']
      if (after) {
        const id = String(after).split(':').slice(1).join(':')
        const index = data.list.findIndex((log) => log.id === id)
        if (index >= 0) data.list = data.list.slice(0, index)
      }
      const latest = data.list[0]
      const cursor = latest
        ? `${Date.parse(latest.occurred_at) * 1000}:${latest.id}`
        : undefined
      emit(
        stream,
        'logs',
        envelope({ ...data, ...(cursor ? { cursor } : {}) }),
        { cursor }
      )
    } catch (error) {
      emit(stream, 'resource-error', errorEnvelope(error.message))
    }
  } else emitSnapshot(stream)
  // Transport keepalive only, without business reads or repeated snapshots.
  const heartbeat = setInterval(() => {
    if (!res.destroyed) res.write(': heartbeat\n\n')
  }, 15000)
  res.once('close', () => {
    clearInterval(heartbeat)
    activeStreams.delete(stream.id)
  })
}
function broadcast(resources) {
  for (const stream of activeStreams.values())
    if (
      !/\/(?:logs|access-logs)\/stream$/.test(stream.url.pathname) &&
      (stream.url.pathname === '/api/overview/stream' ||
        resources.some((resource) =>
          stream.url.pathname.startsWith(`/api/${resource}`)
        ))
    )
      emitSnapshot(stream)
}
function json(res, data, message = 'ok', revision) {
  res.writeHead(200, {
    'Content-Type': 'application/json',
    'X-QA-Fixture': '1',
    ...(revision ? { ETag: `"${revision}"` } : {}),
  })
  res.end(JSON.stringify(envelope(data, message)))
}
async function body(req) {
  let text = ''
  for await (const chunk of req) {
    text += chunk
    if (text.length > 1048576) fail('请求体过大', 413)
  }
  if (!text) return {}
  try {
    return JSON.parse(text)
  } catch {
    fail('请求体不是有效 JSON')
  }
}
function qaState() {
  return {
    active_sse_count: activeStreams.size,
    active_sse_urls: Object.fromEntries(
      [...activeStreams.values()].reduce(
        (counts, stream) =>
          counts.set(
            stream.url.pathname + stream.url.search,
            (counts.get(stream.url.pathname + stream.url.search) ?? 0) + 1
          ),
        new Map()
      )
    ),
    total_requests: totalRequests,
    request_counts: Object.fromEntries(requestCounts),
    emitted_events: emittedEvents,
    streams: [...activeStreams.values()].map((stream) => ({
      id: stream.id,
      url: stream.url.pathname + stream.url.search,
      opened_at: stream.openedAt,
      event_count: stream.eventCount,
      last_event: stream.lastEvent,
    })),
    fixture_counts: {
      nodes: db.nodes.length,
      clusters: db.clusters.length,
      websites: db.websites.length,
      dns_zones: db.zones.length,
      certificates: db.certificates.length,
      cache_policies: db.policies.length,
      tasks: db.tasks.length,
    },
  }
}
function control(query) {
  const event = query.get('event') ?? 'duplicate',
    path = query.get('path')
  const targets = [...activeStreams.values()].filter(
    (stream) => !path || stream.url.pathname.startsWith(path)
  )
  if (event === 'disconnect') {
    for (const stream of targets) stream.res.end()
    return { affected_streams: targets.length }
  }
  if (event === 'duplicate') {
    for (const stream of targets)
      if (stream.lastFrame) {
        stream.res.write(stream.lastFrame)
        stream.eventCount += 1
        emittedEvents += 1
      }
    return { affected_streams: targets.length }
  }
  if (event === 'error') {
    for (const stream of targets)
      emit(stream, 'resource-error', errorEnvelope('QA 场景：连接失败'), {
        force: true,
      })
    return { affected_streams: targets.length }
  }
  if (event === 'reset') {
    db = structuredClone(seed)
    tick = 0
    for (const stream of targets)
      if (!/\/(?:logs|access-logs)\/stream$/.test(stream.url.pathname))
        emitSnapshot(stream, true)
    return { reset: true }
  }
  if (event === 'node-runtime') {
    const node = find(db.nodes, query.get('id') ?? nodeId)
    node.runtime.last_heartbeat_at = nextTime()
    node.runtime.cpu_usage = Number(
      query.get('cpu') ?? (node.runtime.cpu_usage ?? 0) + 1
    )
    for (const stream of targets)
      if (
        stream.url.pathname === '/api/nodes/stream' &&
        (!stream.url.searchParams.get('cluster_id') ||
          stream.url.searchParams.get('cluster_id') === node.cluster_id)
      )
        emit(
          stream,
          'node-runtime',
          { id: node.id, node_revision: node.revision, runtime: node.runtime },
          { force: true }
        )
    return { node_id: node.id, cpu_usage: node.runtime.cpu_usage }
  }
  if (event === 'logs') {
    const resource = query.get('resource') ?? 'node',
      id = query.get('id') ?? (resource === 'node' ? nodeId : websiteId),
      time = nextTime()
    const entry =
      resource === 'node'
        ? {
            id: randomUUID(),
            occurred_at: time,
            level: 'info',
            category: 'qa',
            message: query.get('message') ?? 'QA 事件：收到新的节点运行日志。',
          }
        : {
            ...structuredClone(db.accessLogs[websiteId][0]),
            id: randomUUID(),
            occurred_at: time,
            target: '/qa-event',
          }
    const collection = resource === 'node' ? db.nodeLogs : db.accessLogs
    collection[id] = [entry, ...(collection[id] ?? [])]
    const cursor = `${Date.parse(time) * 1000}:${entry.id}`
    for (const stream of targets)
      if (
        stream.url.pathname ===
        `/api/${resource === 'node' ? 'nodes' : 'websites'}/${id}/${resource === 'node' ? 'logs' : 'access-logs'}/stream`
      )
        emit(stream, 'logs', envelope({ list: [entry], cursor }), { cursor })
    return { log_id: entry.id }
  }
  if (event === 'update') {
    const resource = query.get('resource') ?? 'nodes'
    const collections = {
      nodes: db.nodes,
      clusters: db.clusters,
      websites: db.websites,
      'dns-zones': db.zones,
      certificates: db.certificates,
      'cache-policies': db.policies,
    }
    const collection = collections[resource]
    if (!collection) fail('QA resource 不支持更新')
    const item = find(collection, query.get('id') ?? collection[0]?.id),
      field = query.get('field') ?? 'name'
    if (!['name', 'status'].includes(field))
      fail('QA update 仅支持 name 或 status')
    if (resource === 'websites' && field === 'name')
      item.config.name = query.get('value') ?? 'QA 更新的网站'
    else
      item[field] =
        query.get('value') ??
        (field === 'name' ? `${item.name} · 已更新` : 'disabled')
    touch(item)
    refreshRelations()
    broadcast([resource])
    return { id: item.id, revision: item.revision }
  }
  fail('QA event 不支持')
}
function rejectDelete(resource, id) {
  if (
    resource === 'clusters' &&
    (db.nodes.some((node) => node.cluster_id === id) ||
      db.websites.some((website) => website.cluster_id === id))
  )
    fail('集群有关联节点或网站，无法删除', 409)
  if (
    resource === 'dns-zones' &&
    (db.clusters.some((cluster) => cluster.dns_zone_id === id) ||
      db.certificates.some((certificate) => certificate.dns_zone_id === id) ||
      find(db.zones, id).website_count)
  )
    fail('托管域名正在被使用', 409)
  if (
    resource === 'providers/dns' &&
    db.zones.some((zone) => zone.dns_provider_id === id)
  )
    fail('DNS 供应商存在托管域名，无法删除', 409)
  if (
    resource === 'providers/certificate' &&
    db.certificates.some(
      (certificate) => certificate.certificate_provider_id === id
    )
  )
    fail('证书供应商正在被使用', 409)
  if (
    resource === 'certificates' &&
    db.websites.some((website) => website.config.certificate_ids.includes(id))
  )
    fail('证书正在被网站使用', 409)
  if (
    resource === 'cache-policies' &&
    db.websites.some((website) => website.config.cache_policy_id === id)
  )
    fail('缓存策略正在被网站使用', 409)
}
function createResource(resource, input, url) {
  const base = {
    id: randomUUID(),
    revision: 1,
    created_at: nextTime(),
    updated_at: nextTime(),
  }
  if (resource === 'clusters') {
    find(db.zones, input.dns_zone_id)
    return { ...base, ...input, node_count: 0, online_node_count: 0 }
  }
  if (resource === 'nodes') {
    find(db.clusters, input.cluster_id)
    return {
      ...base,
      ...input,
      node_spec_revision: 1,
      config: {
        ...input.config,
        cache: input.config?.cache ?? structuredClone(cacheConfig),
      },
      runtime: {
        registration_status: 'pending',
        connection_status: 'unregistered',
        applied_node_spec_revision: 0,
      },
    }
  }
  if (resource === 'websites') {
    const id = url.searchParams.get('cluster_id')
    find(db.clusters, id)
    for (const certificate of input.config?.certificate_ids ?? [])
      find(db.certificates, certificate)
    return {
      ...base,
      ...input,
      cluster_id: id,
      certificates: [],
      runtime: {
        domain_states: input.config.domains.map((domain) => ({
          id: domain.id,
          access_protocol: input.config.https_enabled ? 'https' : 'http',
          resolution_status: 'pending',
        })),
        origin_states: [],
        origin_sources: [],
        deploy_status: 'pending',
        target_node_count: 0,
        synced_node_count: 0,
      },
    }
  }
  if (resource === 'dns-zones') {
    const provider = find(db.dnsProviders, input.dns_provider_id)
    return {
      ...base,
      ...input,
      dns_provider: provider.provider,
      sync_status: 'synced',
      desired_revision: 1,
      synced_revision: 1,
      website_count: 0,
      config: { records: [] },
      runtime: {
        records_imported: true,
        lines: structuredClone(lines),
        projected_records: [],
        record_states: [],
        conflicts: [],
      },
      last_synced_at: initialTime,
    }
  }
  if (resource === 'providers/dns')
    return {
      ...base,
      name: input.name,
      provider: input.provider,
      account_id: input.account_id,
      token_hint: `qa…${(input.api_token ?? '').slice(-4)}`,
      status: 'unverified',
      zone_count: 0,
    }
  if (resource === 'providers/certificate')
    return {
      ...base,
      provider: input.provider,
      credential_mode: input.credential_mode,
      ...(input.credential_mode === 'email'
        ? { account_email: input.account_email }
        : { access_key_hint: `qa…${(input.access_key ?? '').slice(-4)}` }),
      status: 'unverified',
    }
  if (resource === 'certificates') {
    const provider = find(
        db.certificateProviders,
        input.certificate_provider_id
      ),
      zone = find(db.zones, input.dns_zone_id)
    return {
      ...base,
      domains: [input.domain],
      certificate_provider_id: provider.id,
      certificate_provider: provider.provider,
      issuer: provider.provider,
      dns_zone_id: zone.id,
      dns_zone_domain: zone.domain,
      status: 'issuing',
      usable: false,
      config: input.config,
      sync_status: 'queued',
      website_count: 0,
    }
  }
  if (resource === 'cache-policies')
    return { ...base, ...input, website_count: 0, websites: [] }
  fail('QA create 不支持')
}
async function mutate(req, res, url) {
  const part = url.pathname.split('/').filter(Boolean),
    resource = part[1] === 'providers' ? `providers/${part[2]}` : part[1],
    id = part[resource.startsWith('providers/') ? 3 : 2],
    action = part[resource.startsWith('providers/') ? 4 : 3]
  const collections = {
      clusters: db.clusters,
      nodes: db.nodes,
      websites: db.websites,
      'dns-zones': db.zones,
      'providers/dns': db.dnsProviders,
      'providers/certificate': db.certificateProviders,
      certificates: db.certificates,
      'cache-policies': db.policies,
    },
    list = collections[resource]
  if (!list) fail(`QA mock 未实现写入：${url.pathname}`, 404)
  const input = await body(req)
  if (resource === 'nodes' && action === 'credentials') {
    const node = find(list, id)
    if (part[4] !== 'reveal') {
      requireRevision(req, node)
      touch(node)
      credentials.set(id, `qa-only-secret-${randomUUID()}`)
      broadcast(['nodes'])
    }
    return json(
      res,
      {
        node_id: id,
        secret: credentials.get(id) ?? `qa-only-secret-${id}`,
        revision: node.revision,
      },
      '凭据已获取',
      node.revision
    )
  }
  if (resource === 'websites' && action === 'cache-jobs') {
    const website = find(db.websites, id)
    if (!part[4]) {
      requireRevision(req, website)
      const job = {
        id: randomUUID(),
        website_id: id,
        website_revision: website.revision,
        policy_revision:
          db.policies.find(
            (policy) => policy.id === website.config.cache_policy_id
          )?.revision ?? 0,
        ...input,
        status: 'queued',
        revision: 1,
        error: '',
        created_at: nextTime(),
        updated_at: nextTime(),
        nodes: db.nodes
          .filter(
            (node) =>
              node.cluster_id === website.cluster_id &&
              node.status === 'enabled'
          )
          .map((node) => ({
            node_id: node.id,
            node_name: node.name,
            command_id: randomUUID(),
            status: 'queued',
            error: '',
            results: [],
          })),
      }
      db.jobs.unshift(job)
      broadcast(['websites'])
      return json(res, job, '缓存任务已创建', 1)
    }
    const job = find(db.jobs, part[4])
    requireRevision(req, job)
    if (part[5] === 'cancel') job.status = 'cancelled'
    else if (part[5] === 'retry') job.status = 'queued'
    else fail('缓存任务操作不正确')
    touch(job)
    broadcast(['websites'])
    return json(res, undefined, '缓存任务已更新', job.revision)
  }
  if (req.method === 'POST' && !id) {
    const item = createResource(resource, input, url)
    list.push(item)
    refreshRelations()
    broadcast([resource, 'clusters', 'cache-policies'])
    if (resource === 'nodes') {
      const secret = `qa-only-secret-${item.id}`
      credentials.set(item.id, secret)
      return json(
        res,
        { node_id: item.id, secret, revision: item.revision },
        '节点已创建',
        item.revision
      )
    }
    return json(res, undefined, '资源已创建', item.revision)
  }
  const item = find(list, id)
  if (req.method === 'POST' && action === 'dns-probe') {
    item.runtime.domain_states = item.config.domains.map((domain) => ({
      id: domain.id,
      access_protocol: item.config.https_enabled ? 'https' : 'http',
      resolution_status: 'resolved',
      last_verified_at: nextTime(),
    }))
    broadcast(['websites'])
    return json(res, undefined, 'DNS 检查已提交')
  }
  if (!(req.method === 'POST' && action === 'sync')) requireRevision(req, item)
  if (req.method === 'DELETE') {
    rejectDelete(resource, id)
    list.splice(list.indexOf(item), 1)
    refreshRelations()
    broadcast([resource, 'clusters', 'cache-policies'])
    return json(res, undefined, '资源已删除', item.revision + 1)
  }
  if (req.method === 'PUT') {
    if (resource === 'websites') {
      const targetCluster =
        url.searchParams.get('cluster_id') ?? item.cluster_id
      find(db.clusters, targetCluster)
      item.cluster_id = targetCluster
      item.status = input.status
      item.config = input.config
    } else if (resource === 'dns-zones') {
      item.config = input
      item.desired_revision = item.revision + 1
      item.synced_revision = item.revision + 1
      item.runtime.record_states = input.records.map((record) => ({
        id: record.id,
        sync_status: 'synced',
        synced_revision: item.revision + 1,
      }))
    } else if (resource === 'certificates')
      item.config = { ...item.config, ...input }
    else if (resource === 'providers/dns') {
      if (input.name) item.name = input.name
      if (input.api_token) {
        item.token_hint = `qa…${input.api_token.slice(-4)}`
        item.status = 'unverified'
      }
    } else if (resource === 'providers/certificate') {
      item.credential_mode = input.credential_mode
      if (input.account_email) item.account_email = input.account_email
      if (input.access_key)
        item.access_key_hint = `qa…${input.access_key.slice(-4)}`
      item.status = 'unverified'
    } else {
      if (resource === 'clusters') find(db.zones, input.dns_zone_id)
      if (resource === 'nodes') {
        find(db.clusters, input.cluster_id)
        item.node_spec_revision += 1
      }
      Object.assign(item, input)
    }
  } else if (req.method === 'POST' && action === 'verify') {
    item.status = 'verified'
    item.last_verified_at = nextTime()
    delete item.last_error
  } else if (req.method === 'POST' && action === 'renew') {
    item.status = 'renewing'
    item.sync_status = 'queued'
  } else if (req.method === 'POST' && action === 'sync') {
    item.sync_status = 'synced'
    item.synced_revision = item.desired_revision
    item.last_synced_at = nextTime()
    item.runtime.conflicts = []
    delete item.last_error
  } else fail('QA 操作未实现', 404)
  if (req.method === 'PUT') touch(item)
  else item.updated_at = nextTime()
  refreshRelations()
  broadcast([resource, 'clusters', 'websites', 'tasks'])
  return json(res, undefined, '资源已更新', item.revision)
}
createServer(async (req, res) => {
  const url = new URL(req.url, 'http://127.0.0.1')
  url.pathname = url.pathname.replace(/\/+$/, '') || '/'
  try {
    if (url.pathname === '/__qa/state') return json(res, qaState())
    if (url.pathname === '/__qa/control')
      return json(res, control(url.searchParams))
    totalRequests += 1
    const key = `${req.method} ${url.pathname}${url.search}`
    requestCounts.set(key, (requestCounts.get(key) ?? 0) + 1)
    if (req.method === 'GET' && url.pathname.endsWith('/stream'))
      return openStream(req, res, url)
    if (url.pathname === '/api/auth/me' || url.pathname === '/api/auth/refresh')
      return json(res, user)
    if (url.pathname === '/api/auth/login') return json(res, { user })
    if (url.pathname === '/api/auth/logout')
      return json(res, undefined, '已退出登录')
    if (
      req.method === 'GET' &&
      /\/certificates\/[^/]+\/download$/.test(url.pathname)
    ) {
      find(db.certificates, url.pathname.split('/')[3])
      res.writeHead(200, {
        'Content-Type': 'application/zip',
        'Content-Disposition':
          'attachment; filename="qa-certificate-fixture.zip"',
        'X-QA-Fixture': '1',
      })
      return res.end(
        Buffer.from('504b0506000000000000000000000000000000000000', 'hex')
      )
    }
    if (['POST', 'PUT', 'DELETE'].includes(req.method))
      return await mutate(req, res, url)
    fail('接口不存在', 404)
  } catch (error) {
    if (res.headersSent) {
      if (!res.destroyed) res.end()
      return
    }
    res.writeHead(error.status ?? 500, {
      'Content-Type': 'application/json',
      'X-QA-Fixture': '1',
    })
    res.end(JSON.stringify(errorEnvelope(error.message, error.code ?? 40001)))
  }
}).listen(Number(process.env.QA_MOCK_PORT ?? 11987), '127.0.0.1', () =>
  console.log(`QA mock listening on ${process.env.QA_MOCK_PORT ?? 11987}`)
)
