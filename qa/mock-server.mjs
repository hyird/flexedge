// Local-only browser QA. Never imported by the production app.
import { randomBytes, randomUUID } from 'node:crypto'
import { readFile } from 'node:fs/promises'
import { createServer } from 'node:http'
import { resolve } from 'node:path'
import { fileURLToPath } from 'node:url'

const root = fileURLToPath(new URL('.', import.meta.url))
const uuid = (number) =>
  `00000000-0000-4000-8000-${String(number).padStart(12, '0')}`
const initialTime = '2026-10-01T01:30:00.000Z'
const timestamps = { created_at: initialTime, updated_at: initialTime }
let tick = 0
const nextTime = () =>
  new Date(Date.parse(initialTime) + ++tick * 1000).toISOString()
const websiteId = uuid(921)
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
const websiteConfig = {
  name: 'QA 网站',
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
      account_id: '0a1b2c3d4e5f60718293a4b5c6d7e8f9',
      token_hint: '****0001',
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
      account_id: 'LTAI5tQaAccount02',
      token_hint: '****0002',
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
      access_key_hint: '****0003',
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
          {
            id: uuid(984),
            type: 'MX',
            name: '@',
            content: 'mx.example.test',
            ttl: 600,
            priority: 10,
            proxied: false,
            line_code: 'default',
          },
        ],
      },
      runtime: {
        records_imported: true,
        lines_synced_at: initialTime,
        lines: structuredClone(lines),
        projected_records: [
          {
            id: uuid(985),
            type: 'CNAME',
            name: 'qa',
            content: 'edge.example.test',
            ttl: 300,
            proxied: false,
            line_code: 'default',
          },
          {
            id: uuid(986),
            type: 'TXT',
            name: '_acme-challenge.qa',
            content: 'qa-acme-token',
            ttl: 120,
            proxied: false,
            line_code: 'default',
          },
        ],
        record_states: [
          { id: uuid(982), sync_status: 'synced', synced_revision: 4 },
          {
            id: uuid(984),
            sync_status: 'failed',
            synced_revision: 3,
            last_error: 'QA 场景：供应商拒绝 MX 记录（优先级冲突）',
          },
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
    {
      id: uuid(987),
      revision: 3,
      dns_provider_id: dnsProviderId,
      dns_provider: 'cloudflare',
      dns_provider_name: 'Cloudflare 生产账号',
      domain: 'conflict.test',
      sync_status: 'conflict',
      desired_revision: 3,
      synced_revision: 2,
      website_count: 0,
      config: {
        records: [
          {
            id: uuid(988),
            type: 'A',
            name: 'www',
            content: '192.0.2.10',
            ttl: 300,
            proxied: false,
            line_code: 'default',
          },
        ],
      },
      runtime: {
        records_imported: true,
        lines: structuredClone(lines),
        projected_records: [],
        record_states: [
          { id: uuid(988), sync_status: 'conflict', synced_revision: 2 },
        ],
        conflicts: [
          {
            id: uuid(988),
            type: 'A',
            name: 'www',
            line_code: 'default',
            local_content: '192.0.2.10',
            remote_content: '198.51.100.7',
          },
        ],
      },
      last_synced_at: initialTime,
      last_error: 'QA 场景：远端记录已被手动修改',
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
      dns_provider_name: 'cloudflare',
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
      dns_provider_name: 'cloudflare',
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
      },
      runtime: {
        registration_status: 'registered',
        connection_status: 'online',
        last_heartbeat_at: initialTime,
        applied_node_spec_revision: 2,
        active_release_id: uuid(701),
        active_manifest_digest: 'a'.repeat(64),
        agent_version: '1.8.0',
        // Ratios in [0, 1], as validated by service/domains/agent/agent_protocol.h.
        cpu_usage: 0.124,
        memory_usage: 0.286,
        traffic_out_bps: 3145728,
        connection_count: 426,
        load_1m: 0.24,
        queued_log_events: 2,
        dropped_log_events: 0,
        health: 'healthy',
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
      },
      runtime: {
        registration_status: 'registered',
        connection_status: 'online',
        last_heartbeat_at: initialTime,
        applied_node_spec_revision: 3,
        agent_version: '1.8.0',
        cpu_usage: 0.082,
        memory_usage: 0.195,
        traffic_out_bps: 1048576,
        connection_count: 187,
        load_1m: 0.12,
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
      domains: ['qa.example.test', 'www.qa.example.test'],
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
    {
      id: uuid(993),
      revision: 3,
      domains: ['shop.example.test'],
      issuer: "Let's Encrypt",
      certificate_provider_id: certificateProviderId,
      certificate_provider: 'letsencrypt',
      status: 'valid',
      usable: true,
      config: { auto_renew: false },
      dns_zone_id: zoneId,
      dns_zone_domain: 'example.test',
      not_before: '2026-07-28T00:00:00Z',
      expires_at: '2026-10-26T00:00:00Z',
      remaining_days: 12,
      serial_number: 'QA000003',
      fingerprint_sha256: 'cd'.repeat(32),
      last_issued_at: '2026-07-28T00:05:00Z',
      sync_status: 'completed',
      sync_count_fails: 0,
      website_count: 0,
      ...timestamps,
    },
    {
      id: uuid(994),
      revision: 2,
      domains: ['legacy.example.test'],
      issuer: 'zerossl',
      certificate_provider_id: uuid(812),
      certificate_provider: 'zerossl',
      status: 'failed',
      usable: false,
      config: { auto_renew: true },
      dns_zone_id: zoneId,
      dns_zone_domain: 'example.test',
      last_error: 'QA 场景：ACME 订单验证失败（DNS-01 记录未生效）',
      sync_status: 'failed',
      sync_count_fails: 3,
      website_count: 0,
      ...timestamps,
    },
    {
      id: uuid(995),
      revision: 4,
      domains: ['old.example.test'],
      issuer: "Let's Encrypt",
      certificate_provider_id: certificateProviderId,
      certificate_provider: 'letsencrypt',
      status: 'expired',
      usable: false,
      config: { auto_renew: false },
      dns_zone_id: zoneId,
      dns_zone_domain: 'example.test',
      not_before: '2026-06-01T00:00:00Z',
      expires_at: '2026-08-30T00:00:00Z',
      remaining_days: 0,
      serial_number: 'QA000005',
      fingerprint_sha256: 'ef'.repeat(32),
      last_issued_at: '2026-06-01T00:05:00Z',
      sync_status: 'completed',
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
        {
          id: certificateId,
          domains: ['qa.example.test', 'www.qa.example.test'],
          usable: true,
        },
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
        deploy_status: 'applied',
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
      },
    ],
  },
}
// Enough nodes and websites to exercise list paging, filters and dashboards.
const generatedNodes = [
  ['QA Edge 04', clusterId, 'online', 0.41, 0.52, 7340032, 'telecom'],
  ['QA Edge 05', clusterId, 'online', 0.67, 0.71, 12582912, 'unicom'],
  ['QA Edge 06', clusterId, 'online', 0.93, 0.88, 20971520, 'default'],
  ['QA Edge 07', clusterId, 'offline', undefined, undefined, 0, 'oversea'],
  ['QA Edge 08', clusterId, 'online', 0.22, 0.34, 2097152, 'telecom'],
  ['QA Edge 09', clusterId, 'online', 0.05, 0.17, 524288, 'default'],
  ['备用 Edge 01', uuid(952), 'offline', undefined, undefined, 0, 'default'],
  [
    '备用 Edge 02',
    uuid(952),
    'unregistered',
    undefined,
    undefined,
    0,
    'unicom',
  ],
  ['QA Edge 10', clusterId, 'online', 0.78, 0.63, 9437184, 'unicom'],
]
generatedNodes.forEach(
  ([name, cluster, connection, cpu, memory, traffic, line], index) => {
    const online = connection === 'online'
    const registered = connection !== 'unregistered'
    db.nodes.push({
      id: uuid(1001 + index),
      revision: 1,
      cluster_id: cluster,
      cluster_name: '',
      name,
      status: registered ? 'enabled' : 'disabled',
      node_spec_revision: 1,
      config: {
        endpoints: [
          {
            id: uuid(1101 + index),
            ip_address: `198.51.100.${40 + index}`,
            line_code: line,
          },
        ],
      },
      runtime: registered
        ? {
            registration_status: 'registered',
            connection_status: connection,
            last_heartbeat_at: new Date(
              Date.parse(initialTime) - (online ? index * 5000 : 3600000)
            ).toISOString(),
            applied_node_spec_revision: 1,
            agent_version: index % 3 === 0 ? '1.7.4' : '1.8.0',
            ...(online
              ? {
                  cpu_usage: cpu,
                  memory_usage: memory,
                  traffic_out_bps: traffic,
                  connection_count: Math.round(traffic / 8192),
                  load_1m: Math.round(cpu * 400) / 100,
                  health: cpu > 0.9 ? 'degraded' : 'healthy',
                }
              : {}),
          }
        : {
            registration_status: 'pending',
            connection_status: 'unregistered',
            applied_node_spec_revision: 0,
          },
      ...timestamps,
    })
  }
)
const generatedWebsites = [
  ['官网', 'www', 'enabled', true, 'applied'],
  ['API 网关', 'api', 'enabled', true, 'applied'],
  ['开发者文档', 'docs', 'enabled', true, 'partial'],
  ['博客', 'blog', 'enabled', true, 'applied'],
  ['下载中心', 'download', 'enabled', false, 'applied'],
  ['控制台前端', 'app', 'enabled', true, 'applied'],
  ['图片服务', 'img', 'enabled', true, 'applied'],
  ['活动页', 'event', 'disabled', false, 'pending'],
  ['帮助中心', 'help', 'enabled', true, 'applied'],
  ['状态页', 'status', 'enabled', false, 'applied'],
  ['视频点播', 'vod', 'enabled', true, 'partial'],
  ['移动端 H5', 'm', 'enabled', true, 'applied'],
  ['合作伙伴门户', 'partner', 'enabled', false, 'pending'],
  ['灰度环境', 'canary', 'disabled', false, 'pending'],
]
generatedWebsites.forEach(([name, host, status, logging, deploy], index) => {
  const domains = [
    {
      id: uuid(1301 + index * 2),
      hostname: `${host}.example.test`,
      dns_mode: 'managed',
    },
    ...(index % 3 === 0
      ? [
          {
            id: uuid(1302 + index * 2),
            hostname: `${host}-cn.example.test`,
            dns_mode: 'external',
          },
        ]
      : []),
  ]
  db.websites.push({
    id: uuid(1201 + index),
    revision: 1,
    cluster_id: clusterId,
    cluster_name: '',
    access_domain: '',
    status,
    config: {
      ...structuredClone(websiteConfig),
      name,
      domains,
      origins: [
        {
          ...structuredClone(websiteConfig.origins[0]),
          id: uuid(1401 + index),
          host: `${host}.origin.qa.test`,
        },
      ],
      access_log_enabled: logging,
      https_enabled: index % 4 !== 3,
      // The first generated website (官网) also binds the QA certificate.
      certificate_ids: index === 0 ? [certificateId] : [],
    },
    certificates: [],
    runtime: {
      domain_states: domains.map((domain) => ({
        id: domain.id,
        access_protocol: index % 4 !== 3 ? 'https' : 'http',
        resolution_status: deploy === 'pending' ? 'pending' : 'verified',
        ...(deploy === 'pending' ? {} : { last_verified_at: initialTime }),
      })),
      origin_sources: [],
      origin_states: [],
      deploy_status: deploy,
      target_node_count: 7,
      synced_node_count:
        deploy === 'applied' ? 7 : deploy === 'partial' ? 4 : 0,
    },
    ...timestamps,
  })
})
db.tasks.push(
  {
    id: 'node:deploy:qa-06',
    resource_type: 'node',
    resource_id: uuid(1003),
    name: 'QA Edge 06',
    operation: 'deploy',
    version: 1,
    status: 'queued',
    error: '',
    failures: 0,
    updated_at: '2026-10-01T01:29:30Z',
    next_attempt_at: '',
  },
  {
    id: 'certificate:renew:qa',
    resource_type: 'certificate',
    resource_id: certificateId,
    name: 'qa.example.test',
    operation: 'renew',
    version: 2,
    status: 'failed',
    error: 'QA 场景：DNS-01 验证超时',
    failures: 3,
    updated_at: '2026-10-01T01:10:00Z',
    next_attempt_at: '',
  },
  {
    id: 'dns-zone:sync:example',
    resource_type: 'dns_zone',
    resource_id: zoneId,
    name: 'example.test',
    operation: 'sync',
    version: 4,
    status: 'recovered',
    error: '',
    failures: 1,
    updated_at: '2026-10-01T00:55:00Z',
    next_attempt_at: '',
  },
  {
    id: 'provider:verify:cloudflare',
    resource_type: 'provider',
    resource_id: dnsProviderId,
    name: 'Cloudflare 生产账号',
    operation: 'verify',
    version: 3,
    status: 'completed',
    error: '',
    failures: 0,
    updated_at: '2026-10-01T00:40:00Z',
    next_attempt_at: '',
  }
)
const seed = structuredClone(db)
// Like the backend: `node_id` is the agent id (32 lowercase hex, not the node
// UUID) and `secret` is 64 lowercase hex; both are regenerated on reset.
const issueCredentials = () => ({
  node_id: randomBytes(16).toString('hex'),
  secret: randomBytes(32).toString('hex'),
})
const credentials = new Map(
  db.nodes.map((node) => [node.id, issueCredentials()])
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
    }
    const code =
      Object.entries(codes).find(([prefix]) => path.startsWith(prefix))?.[1] ??
      40001
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
      // Backend sends the platform code (provider.provider) under this name.
      dns_provider_name: zone?.dns_provider ?? '',
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
// Shared definitions for overview counts and list filters (same 口径).
const zoneSyncStatuses = ['pending', 'syncing', 'synced', 'failed', 'conflict']
const zoneIssueStatuses = ['failed', 'conflict']
const isCertificateExpiring = (certificate) =>
  certificate.status === 'valid' &&
  certificate.remaining_days !== undefined &&
  certificate.remaining_days <= 30
const isCertificateAbnormal = (certificate) =>
  ['failed', 'expired'].includes(certificate.status) ||
  (certificate.status === 'valid' && Boolean(certificate.last_error))
const uuidPattern =
  /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i
// Invalid filter values are rejected before the stream opens (HTTP 400).
function validateStreamQuery(url) {
  const query = url.searchParams
  const invalid = (name) => fail(`参数 ${name} 不合法`, 400, 40001)
  if (url.pathname === '/api/dns-zones/stream') {
    const value = query.get('sync_status')
    if (
      value !== null &&
      value !== 'issue' &&
      !zoneSyncStatuses.includes(value)
    )
      invalid('sync_status')
  }
  if (url.pathname === '/api/certificates/stream')
    for (const name of ['expiring', 'abnormal'])
      if (query.has(name) && query.get(name) !== '1') invalid(name)
  if (url.pathname === '/api/websites/stream') {
    const value = query.get('certificate_id')
    if (value !== null && !uuidPattern.test(value)) invalid('certificate_id')
  }
}
// Overview `nodes` event: same page shape as /api/nodes/stream?page=1&page_size=100.
function overviewNodes() {
  return page(db.nodes, new URLSearchParams({ page: '1', page_size: '100' }))
}
function overview() {
  return {
    resources: {
      website_count: db.websites.length,
      domain_count: db.zones.length,
      certificate_count: db.certificates.length,
      cluster_count: db.clusters.length,
      node_count: db.nodes.length,
      // Backend: registered + enabled + heartbeat within 90 s; the mock keeps
      // fixed heartbeat times, so its connection_status stands in for liveness.
      online_node_count: db.nodes.filter(
        (node) =>
          node.status === 'enabled' &&
          node.runtime.registration_status === 'registered' &&
          node.runtime.connection_status === 'online'
      ).length,
    },
    issues: {
      dns_zone_issue_count: db.zones.filter((zone) =>
        zoneIssueStatuses.includes(zone.sync_status)
      ).length,
      certificate_expiring_count: db.certificates.filter(isCertificateExpiring)
        .length,
      certificate_failed_count: db.certificates.filter(isCertificateAbnormal)
        .length,
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
// Access-log events ingested through /__qa/control?event=logs&resource=website.
const ingestedTraffic = new Map()
const utcHourTimestamp = (time) =>
  new Date(time).toISOString().replace(/\.\d{3}Z$/, '+00:00')
// Overview `traffic` event: 24 ascending hourly points ending at the
// current UTC hour, fixed 1xx–5xx classes, top 8 websites by response bytes,
// computed only from websites with access logging enabled.
function overviewTraffic() {
  const hourMs = 3600000
  const currentHour = Math.floor(Date.now() / hourMs) * hourMs
  const logging = db.websites.filter(
    (website) => website.config.access_log_enabled
  )
  const weights = logging.map((website) => ({
    website,
    weight: 1 + (Number.parseInt(website.id.slice(-4), 10) % 7),
  }))
  const totalWeight = weights.reduce((sum, item) => sum + item.weight, 0)
  const extra = [...ingestedTraffic.values()].reduce(
    (sum, item) => ({
      requests: sum.requests + item.requests,
      bytes: sum.bytes + item.bytes,
      errors: sum.errors + item.errors,
    }),
    { requests: 0, bytes: 0, errors: 0 }
  )
  const hourly = Array.from({ length: 24 }, (_, index) => {
    const timestamp = currentHour - (23 - index) * hourMs
    const hour = new Date(timestamp).getUTCHours()
    const curve = 0.55 + 0.45 * Math.sin(((hour - 8) / 24) * 2 * Math.PI)
    let requestCount = Math.round(totalWeight * 900 * curve)
    let responseBytes = requestCount * 36864
    if (index === 23) {
      requestCount += extra.requests
      responseBytes += extra.bytes
    }
    return {
      timestamp: utcHourTimestamp(timestamp),
      request_count: requestCount,
      response_bytes: responseBytes,
      bandwidth_bps: Math.round((responseBytes * 8) / 3600),
    }
  })
  const requestCount = hourly.reduce(
    (sum, point) => sum + point.request_count,
    0
  )
  const responseBytes = hourly.reduce(
    (sum, point) => sum + point.response_bytes,
    0
  )
  const status5xx = Math.round(requestCount * 0.004) + extra.errors
  const shares = { '1xx': 0.001, '3xx': 0.07, '4xx': 0.065 }
  const classCounts = Object.fromEntries(
    Object.entries(shares).map(([label, share]) => [
      label,
      Math.round(requestCount * share),
    ])
  )
  classCounts['5xx'] = status5xx
  classCounts['2xx'] =
    requestCount -
    Object.values(classCounts).reduce((sum, count) => sum + count, 0)
  const dailyRequests = (weight) =>
    totalWeight
      ? Math.round((requestCount - extra.requests) * (weight / totalWeight))
      : 0
  return {
    window_hours: 24,
    summary: {
      request_count: requestCount,
      response_bytes: responseBytes,
      peak_bandwidth_bps: Math.round(
        Math.max(0, ...hourly.map((point) => point.bandwidth_bps)) * 1.35
      ),
      status_5xx_count: status5xx,
      unique_client_ips: Math.round(requestCount / 37),
    },
    hourly,
    status_classes: ['1xx', '2xx', '3xx', '4xx', '5xx'].map((label) => ({
      label,
      request_count: classCounts[label],
    })),
    top_websites: weights
      .map(({ website, weight }) => {
        const ingested = ingestedTraffic.get(website.id)
        const requests = dailyRequests(weight) + (ingested?.requests ?? 0)
        return {
          website_id: website.id,
          name:
            website.config.name ?? website.config.domains[0]?.hostname ?? '',
          request_count: requests,
          response_bytes:
            Math.round(requests * 36864 * (1 + (weight % 3) * 0.4)) +
            (ingested?.bytes ?? 0),
        }
      })
      .sort((left, right) => right.response_bytes - left.response_bytes)
      .slice(0, 8),
    coverage: {
      logging_website_count: logging.length,
      website_count: db.websites.length,
    },
  }
}
function detail(list, id, code, message) {
  const item = list.find((value) => value.id === id)
  if (!item) fail(message, 404, code)
  return item
}
function readStream(url) {
  refreshRelations()
  const path = url.pathname,
    query = url.searchParams,
    part = path.split('/').filter(Boolean)
  // The combined overview stream is emitted per event in emitSnapshot.
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
  if (path === '/api/dns-zones/stream') {
    const syncStatus = query.get('sync_status')
    return page(
      db.zones.filter(
        (zone) =>
          !syncStatus ||
          (syncStatus === 'issue'
            ? zoneIssueStatuses.includes(zone.sync_status)
            : zone.sync_status === syncStatus)
      ),
      query
    )
  }
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
    return detail(db.zones, part[2], 16201, '托管域名不存在')
  if (path === '/api/clusters/stream') return page(db.clusters, query)
  if (part[1] === 'clusters' && part.length === 4 && part[2] !== 'options')
    return detail(db.clusters, part[2], 16401, '集群不存在')
  if (path === '/api/clusters/options/stream')
    return filtered(db.clusters, query)
  if (path === '/api/nodes/stream') return page(db.nodes, query)
  if (part[1] === 'nodes' && part.length === 4)
    return detail(db.nodes, part[2], 16501, '节点不存在')
  if (part[1] === 'nodes' && part[3] === 'logs') {
    find(db.nodes, part[2])
    return { list: db.nodeLogs[part[2]] ?? [] }
  }
  if (path === '/api/certificates/stream')
    return page(
      db.certificates.filter(
        (certificate) =>
          (query.get('expiring') !== '1' ||
            isCertificateExpiring(certificate)) &&
          (query.get('abnormal') !== '1' || isCertificateAbnormal(certificate))
      ),
      query
    )
  if (path === '/api/certificates/options/stream')
    return filtered(db.certificates, query).filter(
      (value) => query.get('usable') !== 'true' || value.usable
    )
  if (part[1] === 'certificates' && part.length === 4)
    return detail(db.certificates, part[2], 16001, '证书任务不存在')
  if (path === '/api/websites/stream') {
    const certificate = query.get('certificate_id')
    return page(
      db.websites.filter(
        (website) =>
          !certificate || website.config.certificate_ids.includes(certificate)
      ),
      query
    )
  }
  if (part[1] === 'websites') {
    const website = detail(db.websites, part[2], 16601, '网站不存在')
    if (part[3] === 'access-logs')
      return part[4] === 'history'
        ? page(db.accessLogs[part[2]] ?? [], query)
        : { list: db.accessLogs[part[2]] ?? [] }
    if (part[3] === 'dashboard') return dashboard()
    if (part.length === 4) return website
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
  // Deduplicate per event: combined streams only push the parts that changed.
  stream.framesByEvent ??= new Map()
  if (!force && stream.framesByEvent.get(event) === frame) return
  stream.res.write(frame)
  stream.lastFrame = frame
  stream.framesByEvent.set(event, frame)
  stream.eventCount += 1
  emittedEvents += 1
  stream.lastEvent = event
}
function emitSnapshot(stream, force = false) {
  try {
    if (stream.url.pathname === '/api/overview/stream') {
      // Combined overview stream: counts, traffic aggregate and node health.
      refreshRelations()
      emit(stream, 'snapshot', envelope(overview()), { force })
      emit(stream, 'traffic', envelope(overviewTraffic()), { force })
      emit(stream, 'nodes', envelope(overviewNodes()), { force })
      return
    }
    const event = stream.url.pathname.endsWith('/dashboard/stream')
      ? 'dashboard'
      : 'snapshot'
    emit(stream, event, envelope(readStream(stream.url)), { force })
  } catch (error) {
    emit(
      stream,
      'resource-error',
      errorEnvelope(error.message, error.code ?? 40001),
      { force }
    )
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
      emit(
        stream,
        'resource-error',
        errorEnvelope(error.message, error.code ?? 40001)
      )
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
    ingestedTraffic.clear()
    for (const stream of targets)
      if (!/\/(?:logs|access-logs)\/stream$/.test(stream.url.pathname))
        emitSnapshot(stream, true)
    return { reset: true }
  }
  if (event === 'node-runtime') {
    const node = find(db.nodes, query.get('id') ?? nodeId)
    node.runtime.last_heartbeat_at = nextTime()
    node.runtime.cpu_usage = Math.min(
      1,
      Number(query.get('cpu') ?? (node.runtime.cpu_usage ?? 0) + 0.01)
    )
    for (const stream of targets)
      if (
        stream.url.pathname === `/api/nodes/${node.id}/stream` ||
        // Overview `nodes` event holds the first 100 nodes.
        (stream.url.pathname === '/api/overview/stream' &&
          overviewNodes().list.some((item) => item.id === node.id)) ||
        (stream.url.pathname === '/api/nodes/stream' &&
          (!stream.url.searchParams.get('cluster_id') ||
            stream.url.searchParams.get('cluster_id') === node.cluster_id))
      )
        emit(
          stream,
          'node-runtime',
          { id: node.id, node_revision: node.revision, runtime: node.runtime },
          { force: true }
        )
    return { node_id: node.id, cpu_usage: node.runtime.cpu_usage }
  }
  if (event === 'node-connection') {
    // Node goes online/offline: list, detail, cluster and overview snapshots change.
    const node = find(db.nodes, query.get('id') ?? nodeId)
    const status = query.get('status') === 'online' ? 'online' : 'offline'
    node.runtime.connection_status = status
    if (status === 'online') node.runtime.last_heartbeat_at = nextTime()
    refreshRelations()
    broadcast(['nodes', 'clusters'])
    return { node_id: node.id, connection_status: status }
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
    if (resource !== 'node') {
      // Access-log ingestion notifies the tenant-wide traffic aggregate.
      const totals = ingestedTraffic.get(id) ?? {
        requests: 0,
        bytes: 0,
        errors: 0,
      }
      totals.requests += 1
      totals.bytes += entry.response_bytes
      if (entry.status_code >= 500) totals.errors += 1
      ingestedTraffic.set(id, totals)
      for (const stream of targets)
        if (stream.url.pathname === '/api/overview/stream')
          emit(stream, 'traffic', envelope(overviewTraffic()))
    }
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
      config: input.config,
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
      account_id:
        input.provider === 'cloudflare'
          ? input.account_id.toLowerCase()
          : input.account_id,
      token_hint: `****${(input.api_token ?? '').slice(-4)}`,
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
        : { access_key_hint: `****${(input.access_key ?? '').slice(-4)}` }),
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
    },
    list = collections[resource]
  if (!list) fail(`QA mock 未实现写入：${url.pathname}`, 404)
  const input = await body(req)
  if (resource === 'nodes' && action === 'credentials') {
    const node = find(list, id)
    if (part[4] !== 'reveal') {
      requireRevision(req, node)
      touch(node)
      credentials.set(id, issueCredentials())
      broadcast(['nodes'])
    }
    return json(
      res,
      {
        ...(credentials.get(id) ??
          credentials.set(id, issueCredentials()).get(id)),
        revision: node.revision,
      },
      '凭据已获取',
      node.revision
    )
  }
  if (req.method === 'POST' && !id) {
    const item = createResource(resource, input, url)
    list.push(item)
    refreshRelations()
    broadcast([resource, 'clusters'])
    if (resource === 'nodes') {
      const issued = issueCredentials()
      credentials.set(item.id, issued)
      return json(
        res,
        { ...issued, id: item.id, revision: item.revision },
        '节点已创建',
        item.revision
      )
    }
    // Contract: creation responses carry the new resource id.
    if (resource === 'websites')
      return json(
        res,
        { id: item.id, revision: item.revision },
        '资源已创建',
        item.revision
      )
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
    broadcast([resource, 'clusters'])
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
        item.token_hint = `****${input.api_token.slice(-4)}`
        item.status = 'unverified'
      }
    } else if (resource === 'providers/certificate') {
      item.credential_mode = input.credential_mode
      if (input.account_email) item.account_email = input.account_email
      if (input.access_key)
        item.access_key_hint = `****${input.access_key.slice(-4)}`
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
    if (req.method === 'GET' && url.pathname.endsWith('/stream')) {
      validateStreamQuery(url)
      return openStream(req, res, url)
    }
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
