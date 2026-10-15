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
// Website bound to a hosted apex, its `www` and a name in a nested zone.
const brandWebsiteId = uuid(1501),
  brandApexDomainId = uuid(1502),
  brandWwwDomainId = uuid(1503),
  brandImgDomainId = uuid(1504),
  brandZoneId = uuid(1511),
  cdnZoneId = uuid(1514)
const brandApexError =
  '主机记录 @ 已有 A 记录，不能再添加 CNAME；请删除或修改这些记录，或把该网站域名改为外部解析'
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
    // Website apex binding: the CNAME at `@` cannot share its name with the
    // A record there (MX may coexist), so that record and the zone fail.
    {
      id: brandZoneId,
      revision: 3,
      dns_provider_id: dnsProviderId,
      dns_provider: 'cloudflare',
      dns_provider_name: 'Cloudflare 生产账号',
      domain: 'brand.test',
      sync_status: 'failed',
      desired_revision: 3,
      synced_revision: 2,
      website_count: 1,
      config: {
        records: [
          {
            id: uuid(1512),
            type: 'A',
            name: '@',
            content: '203.0.113.20',
            ttl: 600,
            proxied: false,
            line_code: 'default',
          },
          {
            id: uuid(1513),
            type: 'MX',
            name: '@',
            content: 'mx.brand.test',
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
            id: brandApexDomainId,
            type: 'CNAME',
            name: '@',
            content: 'edge.example.test',
            ttl: 600,
            proxied: false,
            line_code: 'default',
          },
          {
            id: brandWwwDomainId,
            type: 'CNAME',
            name: 'www',
            content: 'edge.example.test',
            ttl: 600,
            proxied: false,
            line_code: 'default',
          },
        ],
        record_states: [
          { id: uuid(1512), sync_status: 'synced', synced_revision: 2 },
          { id: uuid(1513), sync_status: 'synced', synced_revision: 2 },
          {
            id: brandApexDomainId,
            sync_status: 'failed',
            synced_revision: 2,
            last_error: brandApexError,
          },
          { id: brandWwwDomainId, sync_status: 'synced', synced_revision: 3 },
        ],
        conflicts: [],
      },
      last_synced_at: initialTime,
      last_error: `1 条记录未同步：@ CNAME：${brandApexError}`,
      ...timestamps,
    },
    // Nested inside example.test: `*.cdn.example.test` names belong here.
    {
      id: cdnZoneId,
      revision: 2,
      dns_provider_id: uuid(802),
      dns_provider: 'aliyun',
      dns_provider_name: '阿里云备用账号',
      domain: 'cdn.example.test',
      sync_status: 'synced',
      desired_revision: 2,
      synced_revision: 2,
      website_count: 1,
      config: { records: [] },
      runtime: {
        records_imported: true,
        lines_synced_at: initialTime,
        lines: structuredClone(lines),
        projected_records: [
          {
            id: brandImgDomainId,
            type: 'CNAME',
            name: 'img',
            content: 'edge.example.test',
            ttl: 600,
            proxied: false,
            line_code: 'default',
          },
        ],
        record_states: [
          { id: brandImgDomainId, sync_status: 'synced', synced_revision: 2 },
        ],
        conflicts: [],
      },
      last_synced_at: initialTime,
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
        connected_ip_count: 213,
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
        // Still on 0.4.0: its heartbeat has no connected_ip_count (UI shows «—»).
        agent_version: '0.4.0',
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
    // Name coverage for the website certificate picker: wildcard, multi-SAN,
    // pending wildcard and a valid certificate no QA website matches.
    ...[
      [996, ['*.example.test'], true],
      [997, ['example.test', 'api.example.test'], true],
      [998, ['*.qa.example.test'], false],
      [999, ['*.other.test'], true],
    ].map(([seed, domains, usable]) => ({
      id: uuid(seed),
      revision: 1,
      domains,
      issuer: "Let's Encrypt",
      certificate_provider_id: certificateProviderId,
      certificate_provider: 'letsencrypt',
      status: usable ? 'valid' : 'issuing',
      usable,
      config: { auto_renew: true },
      dns_zone_id: zoneId,
      dns_zone_domain: 'example.test',
      ...(usable && {
        not_before: '2026-10-01T00:00:00Z',
        expires_at: '2026-12-30T00:00:00Z',
        remaining_days: 77,
        serial_number: `QA000${seed}`,
        fingerprint_sha256: String(seed % 10).repeat(64),
        last_issued_at: initialTime,
      }),
      sync_status: usable ? 'completed' : 'running',
      sync_count_fails: 0,
      website_count: 0,
      ...timestamps,
    })),
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
    {
      id: brandWebsiteId,
      revision: 2,
      cluster_id: clusterId,
      cluster_name: 'QA Cluster',
      access_domain: 'edge.example.test',
      status: 'enabled',
      config: {
        ...structuredClone(websiteConfig),
        name: '品牌官网',
        domains: [
          {
            id: brandApexDomainId,
            hostname: 'brand.test',
            dns_mode: 'managed',
          },
          {
            id: brandWwwDomainId,
            hostname: 'www.brand.test',
            dns_mode: 'managed',
          },
          {
            id: brandImgDomainId,
            hostname: 'img.cdn.example.test',
            dns_mode: 'managed',
          },
        ],
        origins: [
          {
            ...structuredClone(websiteConfig.origins[0]),
            id: uuid(1505),
            host: 'brand.origin.qa.test',
          },
        ],
        certificate_ids: [],
        https_enabled: false,
      },
      certificates: [],
      runtime: {
        domain_states: [
          {
            id: brandApexDomainId,
            access_protocol: 'http',
            resolution_status: 'pending',
          },
          ...[brandWwwDomainId, brandImgDomainId].map((id) => ({
            id,
            access_protocol: 'http',
            resolution_status: 'verified',
            last_verified_at: initialTime,
          })),
        ],
        origin_sources: [],
        origin_states: [],
        deploy_status: 'applied',
        target_node_count: 2,
        synced_node_count: 2,
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
        client_ip_asn: 'AS64500',
        client_ip_as_name:
          'QA Documentation Network for Very Long Autonomous System Names',
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
                  connected_ip_count: Math.round(traffic / 8192 / 3),
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
// Execution history for `/api/tasks/:id/history/stream?version&resource_id`
// (local://task-history-contract v1), keyed by `${task id}|${resource id}`
// and kept out of `db.tasks` so list keyword search ignores it. Times use the
// backend's `…ffffff+00` format, relative to server start so "下次重试"
// counts down in the browser. Covered: DNS diff + conflicts + >100 attempts
// (truncated) + legacy rows, certificate stages, website domain checks, DNS
// and certificate provider verification, node apply (incl. heartbeat); the
// running certificate issue has no attempts yet (empty state).
// `/__qa/control?event=task-history&id=<task>&mode=error|empty|fail|reset`
// (`fail` appends a failed attempt now with the sync retry 15 s later).
// Attempt traces (build/task-trace/contract.md): each history item carries a
// `trace` summary; `/api/tasks/:id/attempts/:attempt_id/trace/stream` serves
// the full trace as a one-shot snapshot (404 for an attempt of another task).
// Covered: Cloudflare find_zone transport error, Aliyun 404 with redacted
// signature parameters (newest 20 of the failure run only, like retention),
// ACME issue/renew with folded polling and a DoH check, website DoH checks,
// DNS / ZeroSSL provider verification, node apply messages; legacy rows and
// heartbeats have `state: none`.
// `/__qa/control?event=task-trace&mode=error|reset` fails every trace read.
const historyBase = Date.now()
const historyStamp = (ms) => new Date(ms).toISOString().replace('Z', '000+00')
const historyAt = (seconds) => historyStamp(historyBase + seconds * 1000)
const historyKey = (task) => `${task.id}|${task.resource_id}`
// Full traces by attempt id; immutable like stored traces, so outside `db`.
const traceDocuments = new Map()
let attemptSequence = 48000
let taskTraceMode = 'reset'
function historyAttempt(
  attempt,
  outcome,
  {
    start,
    duration,
    error = '',
    retry = false,
    detail = { kind: 'none', truncated: false },
    trace,
  }
) {
  // Pre-upgrade records (and node heartbeats) have no start time; their
  // duration is 0 and must not be shown.
  const known = duration !== undefined
  const emitted = start + (duration ?? 0) / 1000
  const attemptId = String(++attemptSequence)
  return {
    attempt,
    outcome,
    error,
    started_at: known ? historyAt(start) : '',
    emitted_at: historyAt(emitted),
    duration_ms: duration ?? 0,
    // Sync tasks retry 15 s after a failure.
    next_attempt_at: retry ? historyAt(emitted + 15) : '',
    detail,
    trace: trace
      ? storeTrace(attemptId, start, trace)
      : traceNone(attemptId),
  }
}
const traceNone = (attemptId) => ({
  state: 'none',
  attempt_id: attemptId,
  step_count: 0,
  call_count: 0,
  log_count: 0,
  truncated: false,
  failed_step: null,
  steps: [],
})
const redacted = '[REDACTED]'
const tracePair = (name, value, hidden = false) => ({
  name,
  value: hidden ? redacted : value,
  redacted: hidden,
})
const pretty = (value) => JSON.stringify(value, null, 2)
function traceBody(
  excerpt,
  {
    format = 'json',
    content_type = 'application/json',
    bytes,
    truncated = false,
    omitted_reason = '',
  } = {}
) {
  return {
    content_type,
    format,
    bytes: bytes ?? Buffer.byteLength(excerpt),
    excerpt,
    truncated,
    omitted_reason,
  }
}
const requestHeaders = (...extra) => [
  tracePair('accept', 'application/json'),
  tracePair('user-agent', 'FlexEdge/2.4'),
  ...extra,
]
/** One outbound HTTP call; `at` is its offset into the step, `repeat` the
 * number of identical requests folded into it. */
function httpCall(
  operation,
  {
    method = 'GET',
    origin,
    path = '/',
    query = [],
    status = 200,
    at = 1,
    duration = 0,
    subject = '',
    request_headers = requestHeaders(),
    request_body = null,
    response_headers = [],
    response_body = null,
    transport_error = '',
    provider_request_id = '',
    provider_code = '',
    repeat = 1,
    every = duration,
    outcome,
    dns = null,
  }
) {
  const url = new URL(origin)
  const search = query.map((item) => `${item.name}=${item.value}`).join('&')
  return {
    kind: dns ? 'dns' : 'http',
    operation,
    subject,
    at,
    duration_ms: duration,
    outcome:
      outcome ??
      (transport_error
        ? 'transport_error'
        : status >= 200 && status < 300
          ? 'ok'
          : 'http_error'),
    repeat_count: repeat,
    repeat_total_ms: duration + (repeat - 1) * every,
    last_at: at + (repeat - 1) * every,
    provider_request_id,
    provider_code,
    error: transport_error,
    http: {
      method,
      url: `${url.origin}${path}${search ? `?${search}` : ''}`,
      scheme: url.protocol.slice(0, -1),
      host: url.host,
      path,
      query,
      request_headers,
      request_body,
      status,
      transport_error,
      response_headers,
      response_body,
    },
    dns,
    node_message: null,
  }
}
function nodeMessage(direction, message, { at = 1, ...fields } = {}) {
  const {
    request_id = '',
    object_count = 0,
    bytes = 0,
    values = [],
    duration = 0,
  } = fields
  return {
    kind: 'node_message',
    // Contract v1.1: node messages use one operation per direction.
    operation: direction === 'sent' ? 'node.send' : 'node.receive',
    subject: '',
    at,
    duration_ms: duration,
    outcome: 'ok',
    repeat_count: 1,
    repeat_total_ms: duration,
    last_at: at,
    provider_request_id: '',
    provider_code: '',
    error: '',
    http: null,
    dns: null,
    node_message: {
      direction,
      message,
      request_id,
      object_count,
      bytes,
      fields: values.map(([name, value]) => ({ name, value })),
    },
  }
}
/** `[code, label, duration, options]`; logs are `[at, level, message, repeat]`. */
const traceStep = (
  code,
  label,
  duration,
  {
    subject = '',
    outcome = 'completed',
    error = '',
    related_task = null,
    calls = [],
    logs = [],
    omitted_calls = 0,
  } = {}
) => ({
  code,
  label,
  duration,
  subject,
  outcome,
  error,
  related_task,
  calls,
  logs,
  omitted_calls,
})
const stepSeverity = { completed: 0, skipped: 1, interrupted: 2, failed: 3 }
function storeTrace(attemptId, start, { kind, worker_type, steps, capture }) {
  const time = (ms) => historyStamp(historyBase + start * 1000 + ms)
  let offset = 2
  const stored = steps.map((step, index) => {
    const base = offset
    offset += step.duration + 1
    return {
      index,
      code: step.code,
      label: step.label,
      subject: step.subject,
      started_at: time(base),
      offset_ms: base,
      duration_ms: step.duration,
      outcome: step.outcome,
      error: step.error,
      related_task: step.related_task,
      call_count:
        step.calls.reduce((total, call) => total + call.repeat_count, 0) +
        step.omitted_calls,
      omitted_calls: step.omitted_calls,
      calls: step.calls.map(({ at, last_at, ...call }, callIndex) => ({
        index: callIndex,
        ...call,
        started_at: time(base + at),
        offset_ms: base + at,
        last_started_at: time(base + last_at),
      })),
      logs: step.logs.map(([at, level, message, repeat = 1]) => ({
        at: time(base + at),
        offset_ms: base + at,
        level,
        message,
        repeat_count: repeat,
      })),
    }
  })
  const serialized = JSON.stringify(stored)
  traceDocuments.set(attemptId, {
    kind,
    worker_type,
    capture: {
      truncated: false,
      degraded: false,
      bytes: Buffer.byteLength(serialized),
      omitted_steps: 0,
      omitted_calls: 0,
      omitted_logs: 0,
      omitted_body_bytes: 0,
      redactions: serialized.split(redacted).length - 1,
      ...capture,
    },
    steps: stored,
  })
  // History summary: adjacent steps with the same code merge (worst outcome,
  // summed duration and calls).
  const merged = []
  for (const step of stored) {
    const last = merged.at(-1)
    if (last?.code === step.code) {
      last.count += 1
      last.duration_ms += step.duration_ms
      last.call_count += step.call_count
      if (stepSeverity[step.outcome] > stepSeverity[last.outcome])
        last.outcome = step.outcome
    } else
      merged.push({
        code: step.code,
        label: step.label,
        outcome: step.outcome,
        duration_ms: step.duration_ms,
        call_count: step.call_count,
        count: 1,
      })
  }
  const failed = stored.find((step) => step.outcome === 'failed')
  return {
    state: 'available',
    attempt_id: attemptId,
    step_count: stored.length,
    call_count: stored.reduce((total, step) => total + step.call_count, 0),
    log_count: stored.reduce((total, step) => total + step.logs.length, 0),
    truncated: Boolean(capture?.truncated) || merged.length > 24,
    failed_step: failed ? { code: failed.code, label: failed.label } : null,
    steps: merged.slice(0, 24),
  }
}
const workerTypes = {
  dns_zone: 'dns-sync',
  certificate: 'certificate',
  website: 'website-dispatch',
  provider: 'provider-verification',
  node: 'web',
}
/** Full trace snapshot: attempt fields from the history row, steps stored. */
function traceSnapshot(task, item) {
  const stored = traceDocuments.get(item.trace.attempt_id)
  return {
    attempt_id: item.trace.attempt_id,
    task_id: task.id,
    resource_type: task.resource_type,
    resource_id: task.resource_id,
    kind: stored?.kind ?? item.detail.kind,
    state: stored ? 'available' : 'none',
    attempt: item.attempt,
    outcome: item.outcome,
    error: item.error,
    started_at: item.started_at,
    emitted_at: item.emitted_at,
    duration_ms: item.duration_ms,
    worker_type: stored?.worker_type ?? workerTypes[task.resource_type] ?? '',
    capture: stored?.capture ?? {
      truncated: false,
      degraded: false,
      bytes: 0,
      omitted_steps: 0,
      omitted_calls: 0,
      omitted_logs: 0,
      omitted_body_bytes: 0,
      redactions: 0,
    },
    steps: stored?.steps ?? [],
  }
}
function historySnapshot(task, attempts) {
  const failures = attempts.filter((item) => item.outcome === 'failed')
  return {
    truncated: attempts.length > 100,
    summary: {
      attempt_count: attempts.length,
      failure_count: failures.length,
      first_failure_at: failures.at(-1)?.emitted_at ?? '',
      last_success_at:
        attempts.find((item) => item.outcome === 'completed')?.emitted_at ?? '',
      next_attempt_at: task.status === 'retrying' ? task.next_attempt_at : '',
    },
    list: attempts.slice(0, 100),
  }
}
const dnsRecord = (
  name,
  type,
  content,
  { ttl = 600, line_code = 'default', priority = null, proxied = false } = {}
) => ({ name, type, line_code, content, ttl, priority, proxied })
function dnsDetail({
  created = [],
  updated = [],
  deleted = [],
  conflicts = [],
  ...counts
} = {}) {
  return {
    kind: 'dns_sync',
    truncated: false,
    created,
    updated,
    deleted,
    created_count: created.length,
    updated_count: updated.length,
    deleted_count: deleted.length,
    unchanged_count: 18,
    conflicts,
    conflict_count: conflicts.length,
    ...counts,
  }
}
const dnsDiff = dnsDetail({
  created: [
    dnsRecord('static', 'CNAME', 'edge-qa.example.test'),
    dnsRecord('_acme-challenge.static', 'TXT', 'Zq8m1y2QvB7oP0xKk3sNw4rT5uV6', {
      ttl: 120,
    }),
  ],
  updated: [
    {
      before: dnsRecord('www', 'A', '203.0.113.10', { line_code: 'telecom' }),
      after: dnsRecord('www', 'A', '203.0.113.24', {
        ttl: 300,
        line_code: 'telecom',
      }),
    },
    {
      before: dnsRecord('@', 'MX', 'mx1.example.test', { priority: 10 }),
      after: dnsRecord('@', 'MX', 'mx2.example.test', { priority: 5 }),
    },
  ],
  deleted: [dnsRecord('old-api', 'AAAA', '2001:db8::17')],
  unchanged_count: 14,
})
const certificateDetail = (operation, stage, success = {}) => ({
  kind: 'certificate',
  truncated: false,
  operation,
  provider: 'letsencrypt',
  domains: ['qa.example.test', 'www.qa.example.test'],
  stage,
  issuer: '',
  not_before: '',
  not_after: '',
  serial_number: '',
  fingerprint_sha256: '',
  ...success,
})
const domainResult = (hostname, status, resolved, error = '') => ({
  hostname,
  query_name: hostname.startsWith('*.')
    ? `flexedge-3f9a1c2e.${hostname.slice(2)}`
    : hostname,
  expected: 'edge-qa.example.test',
  resolved,
  status,
  error,
})
const domainsDetail = (domains, checked = domains.length) => ({
  kind: 'website_domains',
  truncated: false,
  expected_target: 'edge-qa.example.test',
  domain_count: 4,
  checked_count: checked,
  domains,
})
const nodeApply = (patch) => ({
  kind: 'node_apply',
  truncated: false,
  node_id: uuid(1003),
  node_name: 'QA Edge 06',
  agent_version: '2.4.1',
  source: 'apply_result',
  result: 'applied',
  phase: '',
  error_code: '',
  retryable: false,
  ...patch,
})
const providerDetail = (patch) => ({
  kind: 'provider',
  truncated: false,
  provider: 'cloudflare',
  provider_kind: 'dns',
  zone_count: 0,
  zones: [],
  directory_url: '',
  external_account: '',
  ...patch,
})
// Trace scenarios, one builder per task kind.
const cloudflareApi = 'https://api.cloudflare.com'
const cloudflareZone = '8c1f6d2e4b3a59c7d0e1f2a3b4c5d6e7'
const cfRay = (suffix) => tracePair('cf-ray', `8d2c4f1a9b7e${suffix}-HKG`)
const cloudflareAuth = requestHeaders(tracePair('authorization', '', true))
const syncPreamble = () => [
  traceStep('load_state', '加载同步状态', 9),
  traceStep('open_credential', '解密凭据', 0),
  traceStep('validate_ttl', '校验 TTL', 0),
]
const findCloudflareZone = (domain, options = {}) =>
  httpCall('cloudflare.find_zone', {
    origin: cloudflareApi,
    path: '/client/v4/zones',
    query: [tracePair('name', domain), tracePair('status', 'active')],
    subject: domain,
    request_headers: cloudflareAuth,
    ...options,
  })
function cloudflareSyncFailureTrace() {
  return {
    kind: 'dns_sync',
    worker_type: 'dns-sync',
    steps: [
      ...syncPreamble(),
      traceStep('find_zone', '查找远程托管区域', 5012, {
        subject: 'example.test',
        outcome: 'failed',
        error: 'Cloudflare API 连接失败',
        calls: [
          findCloudflareZone('example.test', {
            status: 0,
            duration: 5003,
            transport_error: 'connect_failed',
          }),
        ],
        logs: [[5006, 'warning', 'Cloudflare API 连接失败，15 秒后重试']],
      }),
      traceStep('record_failure', '记录失败', 4, {
        logs: [[3, 'error', '同步失败（可重试）：Cloudflare API 连接失败']],
      }),
    ],
  }
}
function cloudflareSyncTrace() {
  const zone = `/client/v4/zones/${cloudflareZone}/dns_records`
  const created = Array.from({ length: 8 }, (_, index) =>
    httpCall('cloudflare.create_record', {
      method: 'POST',
      origin: cloudflareApi,
      path: zone,
      at: 2 + index * 28,
      duration: 24 + (index % 3) * 6,
      subject: `node-${index + 1} A`,
      request_headers: [
        ...cloudflareAuth,
        tracePair('content-type', 'application/json'),
      ],
      request_body: traceBody(
        pretty({
          content: `203.0.113.${index + 31}`,
          name: `node-${index + 1}.example.test`,
          proxied: false,
          ttl: 600,
          type: 'A',
        })
      ),
      response_headers: [
        tracePair('content-type', 'application/json'),
        cfRay(`${index}0`),
      ],
      provider_request_id: `8d2c4f1a9b7e${index}0-HKG`,
      response_body: traceBody(
        pretty({
          result: {
            id: `${index + 1}f0e9d8c7b6a5948372615a4b3c2d1e0`,
            name: `node-${index + 1}.example.test`,
            type: 'A',
          },
          success: true,
        })
      ),
    })
  )
  return {
    kind: 'dns_sync',
    worker_type: 'dns-sync',
    capture: { truncated: true, omitted_calls: 56 },
    steps: [
      ...syncPreamble(),
      traceStep('find_zone', '查找远程托管区域', 212, {
        subject: 'example.test',
        calls: [
          findCloudflareZone('example.test', {
            duration: 208,
            response_headers: [
              tracePair('content-type', 'application/json'),
              cfRay('a1'),
            ],
            provider_request_id: '8d2c4f1a9b7ea1-HKG',
            response_body: traceBody(
              pretty({
                result: [
                  { id: cloudflareZone, name: 'example.test', status: 'active' },
                ],
                result_info: { count: 1, page: 1, total_count: 1 },
                success: true,
              })
            ),
          }),
        ],
      }),
      traceStep('list_records', '拉取远程记录', 640, {
        calls: [1, 2].map((page) =>
          httpCall('cloudflare.list_records', {
            origin: cloudflareApi,
            path: zone,
            query: [
              tracePair('page', String(page)),
              tracePair('per_page', '100'),
            ],
            at: page === 1 ? 1 : 330,
            duration: page === 1 ? 318 : 290,
            request_headers: cloudflareAuth,
            response_headers: [
              tracePair('content-type', 'application/json'),
              cfRay(`b${page}`),
            ],
            provider_request_id: `8d2c4f1a9b7eb${page}-HKG`,
            response_body: traceBody(
              `{\n  "result": [\n    {\n      "content": "203.0.113.24",\n      "id": "0c9d8e7f6a5b4c3d2e1f0a9b8c7d6e5f",\n      "name": "www.example.test",\n      "proxied": false,\n      "ttl": 300,\n      "type": "A"\n    },\n    {\n      "content": "edge-qa.example.test",\n      "id": "1d2e3f4a5b6c7d8e9f0a1b2c3d4e5f6a",\n      "name": "static.example.test",\n      "proxied": false,\n      "ttl": 600,\n      "type": "CNAME"\n    },`,
              { bytes: 41_872, truncated: true }
            ),
          })
        ),
        logs: [[628, 'info', '远程记录共 112 条（2 页）']],
      }),
      traceStep('list_lines', '拉取解析线路', 0, {
        outcome: 'skipped',
        logs: [[0, 'info', 'Cloudflare 只有默认线路，跳过']],
      }),
      traceStep('merge_remote', '合并远程变更', 6),
      traceStep('reconcile_records', '同步期望记录', 1840, {
        calls: created,
        omitted_calls: 56,
        logs: [
          [1, 'info', '10 条记录已一致'],
          [1836, 'info', '新增 64 条记录'],
        ],
      }),
      traceStep('persist_result', '保存同步结果', 15),
    ],
  }
}
function aliyunSyncFailureTrace() {
  const requestId = '5E7C2A1B-93F4-4D8A-B6E2-1C0F9A7D3B54'
  return {
    kind: 'dns_sync',
    worker_type: 'dns-sync',
    steps: [
      ...syncPreamble(),
      traceStep('find_zone', '查找远程托管区域', 186, {
        subject: 'backup.test',
        outcome: 'failed',
        error:
          'DescribeDomains 返回 InvalidAccessKeyId.NotFound：Specified access key is not found.',
        calls: [
          httpCall('aliyun.describe_domains', {
            origin: 'https://alidns.aliyuncs.com',
            query: [
              tracePair('AccessKeyId', '', true),
              tracePair('Action', 'DescribeDomains'),
              tracePair('Format', 'JSON'),
              tracePair('KeyWord', 'backup.test'),
              tracePair('PageNumber', '1'),
              tracePair('PageSize', '100'),
              tracePair('Signature', '', true),
              tracePair('SignatureMethod', 'HMAC-SHA1'),
              tracePair('SignatureNonce', '', true),
              tracePair('SignatureVersion', '1.0'),
              tracePair('Timestamp', '2026-10-14T14%3A16%3A44Z'),
              tracePair('Version', '2015-01-09'),
            ],
            subject: 'backup.test',
            status: 404,
            duration: 181,
            provider_request_id: requestId,
            provider_code: 'InvalidAccessKeyId.NotFound',
            response_headers: [
              tracePair('content-type', 'application/json;charset=utf-8'),
              tracePair('x-acs-request-id', requestId),
              tracePair('set-cookie', '', true),
            ],
            response_body: traceBody(
              pretty({
                Code: 'InvalidAccessKeyId.NotFound',
                HostId: 'alidns.aliyuncs.com',
                Message: 'Specified access key is not found.',
                Recommend:
                  'https://api.aliyun.com/troubleshoot?q=InvalidAccessKeyId.NotFound&product=Alidns',
                RequestId: requestId,
              })
            ),
          }),
        ],
        logs: [[183, 'warning', '阿里云拒绝访问密钥，15 秒后重试']],
      }),
      traceStep('record_failure', '记录失败', 5, {
        logs: [[4, 'error', '同步失败（可重试）：访问密钥不存在']],
      }),
    ],
  }
}
const letsEncrypt = 'https://acme-v02.api.letsencrypt.org'
const acmeAccount = `${letsEncrypt}/acme/acct/1789320`
const nonceHeaders = () => [
  tracePair('replay-nonce', '', true),
  tracePair('cache-control', 'no-store'),
]
const jws = (url, payload) =>
  traceBody(
    pretty({
      protected: { alg: 'ES256', kid: acmeAccount, url },
      payload,
    }),
    { format: 'jws_decoded', content_type: 'application/jose+json' }
  )
const acmeDirectory = (at) =>
  httpCall('acme.directory', {
    origin: letsEncrypt,
    path: '/directory',
    at,
    duration: 142,
    response_headers: [tracePair('content-type', 'application/json')],
    response_body: traceBody(
      pretty({
        keyChange: `${letsEncrypt}/acme/key-change`,
        meta: {
          caaIdentities: ['letsencrypt.org'],
          termsOfService:
            'https://letsencrypt.org/documents/LE-SA-v1.5-February-24-2025.pdf',
        },
        newAccount: `${letsEncrypt}/acme/new-acct`,
        newNonce: `${letsEncrypt}/acme/new-nonce`,
        newOrder: `${letsEncrypt}/acme/new-order`,
        revokeCert: `${letsEncrypt}/acme/revoke-cert`,
      })
    ),
  })
const acmeNonce = (at, repeat = 1) =>
  httpCall('acme.new_nonce', {
    method: 'HEAD',
    origin: letsEncrypt,
    path: '/acme/new-nonce',
    at,
    duration: 38,
    repeat,
    every: 2100,
    response_headers: nonceHeaders(),
  })
const authzPath = (index) => `/acme/authz/237841${index}/5678${index}`
function acmeAuthorization(index, status, { at = 1, repeat = 1 } = {}) {
  const domain = index ? 'www.qa.example.test' : 'qa.example.test'
  return httpCall('acme.authorization', {
    method: 'POST',
    origin: letsEncrypt,
    path: authzPath(index),
    at,
    duration: 96,
    repeat,
    every: 2000,
    subject: domain,
    request_headers: requestHeaders(
      tracePair('content-type', 'application/jose+json')
    ),
    request_body: jws(`${letsEncrypt}${authzPath(index)}`, '(POST-as-GET)'),
    response_headers: nonceHeaders(),
    response_body: traceBody(
      pretty({
        challenges: [
          {
            status,
            token: redacted,
            type: 'dns-01',
            url: `${letsEncrypt}/acme/chall/237841${index}/5678${index}/Ab12`,
          },
        ],
        expires: '2026-10-21T14:16:44Z',
        identifier: { type: 'dns', value: domain },
        status,
      })
    ),
  })
}
const relatedSync = () => ({
  task_id: 'dns-zone:sync:example',
  resource_type: 'dns_zone',
  resource_id: zoneId,
  label: 'DNS 同步',
})
function challengeCheck(at, repeat, data) {
  const name = '_acme-challenge.qa.example.test'
  return httpCall('doh.resolve', {
    origin: 'https://dns.alidns.com',
    path: '/resolve',
    query: [
      tracePair('name', name),
      tracePair('type', 'TXT'),
      tracePair('cd', '0'),
      tracePair('do', '0'),
    ],
    at,
    duration: 31,
    repeat,
    every: 8000,
    subject: name,
    response_headers: [tracePair('content-type', 'application/dns-json')],
    response_body: traceBody(
      pretty({
        Answer: [{ TTL: 120, data, name: `${name}.`, type: 16 }],
        Status: 0,
      }),
      { content_type: 'application/dns-json' }
    ),
    dns: {
      resolver: 'dns.alidns.com',
      transport: 'doh_json',
      name,
      type: 'TXT',
      rcode: 'NOERROR',
      answers: [{ name: `${name}.`, type: 'TXT', ttl: 120, data }],
      truncated: false,
    },
  })
}
const certificatePrepare = (operation) =>
  traceStep('prepare', '准备签发', 21, {
    logs: [
      [
        5,
        'info',
        `${operation === 'renew' ? '续期' : '签发'}证书：qa.example.test、www.qa.example.test`,
      ],
    ],
  })
const certificateOrder = () =>
  traceStep('order', '创建订单', 455, {
    calls: [
      acmeNonce(1),
      httpCall('acme.new_order', {
        method: 'POST',
        origin: letsEncrypt,
        path: '/acme/new-order',
        at: 42,
        duration: 402,
        status: 201,
        request_headers: requestHeaders(
          tracePair('content-type', 'application/jose+json')
        ),
        request_body: jws(`${letsEncrypt}/acme/new-order`, {
          identifiers: [
            { type: 'dns', value: 'qa.example.test' },
            { type: 'dns', value: 'www.qa.example.test' },
          ],
        }),
        response_headers: [
          tracePair('location', `${letsEncrypt}/acme/order/1789320/30419875`),
          ...nonceHeaders(),
        ],
        response_body: traceBody(
          pretty({
            authorizations: [0, 1].map(
              (index) => `${letsEncrypt}${authzPath(index)}`
            ),
            finalize: `${letsEncrypt}/acme/finalize/1789320/30419875`,
            status: 'pending',
          })
        ),
      }),
    ],
  })
const certificateAuthorization = () =>
  traceStep('authorization', '获取域名授权', 210, {
    calls: [
      acmeAuthorization(0, 'pending'),
      acmeAuthorization(1, 'pending', { at: 104 }),
    ],
  })
function certificateRenewFailureTrace(wait) {
  return {
    kind: 'certificate',
    worker_type: 'certificate',
    steps: [
      certificatePrepare('renew'),
      traceStep('account', 'ACME 账户', 190, {
        calls: [acmeDirectory(1), acmeNonce(150)],
        logs: [[188, 'info', '复用已注册的 ACME 账户']],
      }),
      certificateOrder(),
      certificateAuthorization(),
      traceStep('dns_challenge', '写入 DNS 验证记录并等待生效', wait, {
        subject: '_acme-challenge.qa.example.test',
        outcome: 'failed',
        error:
          'DNS-01 验证超时：_acme-challenge.qa.example.test 的 TXT 记录在 120 秒内未在权威服务器上生效。',
        related_task: relatedSync(),
        calls: [challengeCheck(10_200, 14, `"${redacted}"`)],
        logs: [
          [8, 'info', '已写入 2 条验证记录，等待 DNS 同步任务完成'],
          [10_100, 'info', 'DNS 同步已完成，开始检查公共解析'],
          [18_300, 'warning', 'TXT 记录尚未生效：解析结果与期望值不一致', 14],
          [wait - 4, 'error', '120 秒内 TXT 记录未生效，停止等待'],
        ],
      }),
      traceStep('cleanup', '清理 DNS 验证记录', 3100, {
        related_task: relatedSync(),
        logs: [[3096, 'info', '已删除 2 条验证记录']],
      }),
      traceStep('record_failure', '记录失败', 6, {
        logs: [[5, 'error', '续期失败，将在下一个续期窗口重试']],
      }),
    ],
  }
}
function certificateIssueTrace() {
  const order = `${letsEncrypt}/acme/order/1789320/30419875`
  const orderPoll = (status, at, repeat) =>
    httpCall('acme.order_poll', {
      method: 'POST',
      origin: letsEncrypt,
      path: '/acme/order/1789320/30419875',
      at,
      duration: 88,
      repeat,
      every: 2000,
      request_body: jws(order, '(POST-as-GET)'),
      response_headers: nonceHeaders(),
      response_body: traceBody(pretty({ status })),
    })
  return {
    kind: 'certificate',
    worker_type: 'certificate',
    steps: [
      certificatePrepare('issue'),
      traceStep('account', 'ACME 账户', 980, {
        calls: [
          acmeDirectory(1),
          acmeNonce(150),
          httpCall('acme.new_account', {
            method: 'POST',
            origin: letsEncrypt,
            path: '/acme/new-acct',
            at: 196,
            duration: 780,
            status: 201,
            request_body: traceBody(
              pretty({
                protected: {
                  alg: 'ES256',
                  jwk: { crv: 'P-256', kty: 'EC', x: redacted, y: redacted },
                  url: `${letsEncrypt}/acme/new-acct`,
                },
                payload: {
                  contact: [`mailto:${redacted}`],
                  termsOfServiceAgreed: true,
                },
              }),
              { format: 'jws_decoded', content_type: 'application/jose+json' }
            ),
            response_headers: [
              tracePair('location', acmeAccount),
              ...nonceHeaders(),
            ],
            response_body: traceBody(
              pretty({ contact: [`mailto:${redacted}`], status: 'valid' })
            ),
          }),
        ],
        logs: [[978, 'info', '已注册新的 ACME 账户']],
      }),
      certificateOrder(),
      certificateAuthorization(),
      traceStep('dns_challenge', '写入 DNS 验证记录并等待生效', 31_000, {
        subject: '_acme-challenge.qa.example.test',
        related_task: relatedSync(),
        calls: [challengeCheck(10_400, 3, `"${redacted}"`)],
        logs: [
          [8, 'info', '已写入 2 条验证记录，等待 DNS 同步任务完成'],
          [20_900, 'info', 'TXT 记录已在公共解析生效，固定等待 10 秒'],
        ],
      }),
      traceStep('validation', '证书颁发机构验证', 22_000, {
        calls: [
          ...[0, 1].map((index) =>
            httpCall('acme.challenge', {
              method: 'POST',
              origin: letsEncrypt,
              path: `/acme/chall/237841${index}/5678${index}/Ab12`,
              at: 1 + index * 120,
              duration: 110,
              request_body: jws(
                `${letsEncrypt}/acme/chall/237841${index}/5678${index}/Ab12`,
                {}
              ),
              response_headers: nonceHeaders(),
              response_body: traceBody(
                pretty({ status: 'processing', type: 'dns-01' })
              ),
            })
          ),
          acmeNonce(260, 11),
          acmeAuthorization(0, 'pending', { at: 300, repeat: 9 }),
          acmeAuthorization(0, 'valid', { at: 18_400 }),
          acmeAuthorization(1, 'valid', { at: 18_600 }),
        ],
        logs: [
          [300, 'info', '授权状态 pending，2 秒后再次检查', 9],
          [18_700, 'info', '全部授权已生效'],
        ],
      }),
      traceStep('cleanup', '清理 DNS 验证记录', 2800, {
        related_task: relatedSync(),
      }),
      traceStep('finalize', '提交证书请求并等待签发', 9000, {
        calls: [
          httpCall('acme.finalize', {
            method: 'POST',
            origin: letsEncrypt,
            path: '/acme/finalize/1789320/30419875',
            duration: 640,
            request_body: traceBody('', {
              format: 'omitted',
              content_type: 'application/jose+json',
              bytes: 1312,
              omitted_reason: 'policy',
            }),
            response_headers: nonceHeaders(),
            response_body: traceBody(pretty({ status: 'processing' })),
          }),
          orderPoll('processing', 700, 3),
          orderPoll('valid', 6800, 1),
        ],
      }),
      traceStep('download', '下载并校验证书', 640, {
        calls: [
          httpCall('acme.certificate', {
            method: 'POST',
            origin: letsEncrypt,
            path: '/acme/cert/04a1f3c2b9d87e6510fa33c1d2e4b5a69c70',
            duration: 610,
            request_body: jws(
              `${letsEncrypt}/acme/cert/04a1f3c2b9d87e6510fa33c1d2e4b5a69c70`,
              '(POST-as-GET)'
            ),
            response_headers: [
              tracePair('content-type', 'application/pem-certificate-chain'),
              ...nonceHeaders(),
            ],
            response_body: traceBody(
              pretty([
                {
                  issuer: "CN=R11,O=Let's Encrypt,C=US",
                  not_after: '2026-11-13T07:12:43Z',
                  serial_number: '04A1F3C2B9D87E6510FA33C1D2E4B5A69C70',
                  subject: 'CN=qa.example.test',
                },
                {
                  issuer: 'CN=ISRG Root X1,O=Internet Security Research Group,C=US',
                  not_after: '2027-03-12T23:59:59Z',
                  serial_number: '8A7D3E13D62F30EF2386BD29076B34F8',
                  subject: "CN=R11,O=Let's Encrypt,C=US",
                },
              ]),
              {
                format: 'pem_summary',
                content_type: 'application/pem-certificate-chain',
                bytes: 3826,
              }
            ),
          }),
        ],
        logs: [[630, 'info', '证书 SAN 与申请域名一致']],
      }),
      traceStep('persist', '保存证书', 40, {
        logs: [[38, 'info', '已通知 3 个节点发布消费者']],
      }),
    ],
  }
}
function domainCheck(hostname, target, { at = 1, outcome } = {}) {
  const name = hostname.startsWith('*.')
    ? `flexedge-3f9a1c2e.${hostname.slice(2)}`
    : hostname
  const failed = outcome === 'timeout'
  return httpCall('doh.resolve', {
    origin: 'https://dns.alidns.com',
    path: '/resolve',
    query: [
      tracePair('name', name),
      tracePair('type', 'CNAME'),
      tracePair('cd', '0'),
      tracePair('do', '0'),
    ],
    at,
    duration: failed ? 5000 : 28,
    subject: hostname,
    status: failed ? 0 : 200,
    transport_error: failed ? 'timeout' : '',
    response_headers: failed
      ? []
      : [tracePair('content-type', 'application/dns-json')],
    response_body: failed
      ? null
      : traceBody(
          pretty({
            Answer: [{ TTL: 600, data: `${target}.`, name: `${name}.`, type: 5 }],
            Status: 0,
          }),
          { content_type: 'application/dns-json' }
        ),
    dns: {
      resolver: 'dns.alidns.com',
      transport: 'doh_json',
      name,
      type: 'CNAME',
      rcode: failed ? '' : 'NOERROR',
      answers: failed
        ? []
        : [{ name: `${name}.`, type: 'CNAME', ttl: 600, data: `${target}.` }],
      truncated: false,
    },
  })
}
function websiteCheckTrace(results) {
  return {
    kind: 'website_domains',
    worker_type: 'website-dispatch',
    steps: [
      traceStep('load_website', '读取网站配置', 7),
      ...results.map(([hostname, target, outcome]) =>
        traceStep('check_domain', '检测域名解析', outcome === 'timeout' ? 5003 : 31, {
          subject: hostname,
          outcome: outcome === 'timeout' ? 'failed' : 'completed',
          error: outcome === 'timeout' ? '公共 DNS 查询超时' : '',
          calls: [domainCheck(hostname, target, { outcome })],
          logs:
            target === 'edge-qa.example.test' || outcome === 'timeout'
              ? []
              : [
                  [
                    30,
                    'warning',
                    `期望 edge-qa.example.test，实际 ${target}`,
                  ],
                ],
        })
      ),
      traceStep('save_runtime', '保存解析结果', 12),
    ],
  }
}
function cloudflareVerificationTrace(failed) {
  const listZones = httpCall('cloudflare.list_zones', {
    origin: cloudflareApi,
    path: '/client/v4/zones',
    query: [tracePair('page', '1'), tracePair('per_page', '50')],
    at: 150,
    duration: failed ? 162 : 231,
    status: failed ? 403 : 200,
    request_headers: cloudflareAuth,
    provider_request_id: '8d2c4f1a9b7ec2-HKG',
    provider_code: failed ? '9109' : '',
    response_headers: [
      tracePair('content-type', 'application/json'),
      cfRay('c2'),
    ],
    response_body: traceBody(
      pretty(
        failed
          ? {
              errors: [
                {
                  code: 9109,
                  message: 'Unauthorized to access requested resource',
                },
              ],
              success: false,
            }
          : {
              result: [
                { id: cloudflareZone, name: 'example.test', status: 'active' },
                { id: redacted, name: 'backup.test', status: 'active' },
                {
                  id: redacted,
                  name: 'staging.example.test',
                  status: 'pending',
                },
              ],
              result_info: { count: 3, page: 1, total_count: 3 },
              success: true,
            }
      )
    ),
  })
  return {
    kind: 'provider',
    worker_type: 'provider-verification',
    steps: [
      traceStep('load_provider', '读取供应商状态', 6),
      traceStep('decrypt_credential', '解密凭据', 0),
      traceStep('verify_dns_credential', '校验 DNS 服务商凭据', failed ? 318 : 392, {
        outcome: failed ? 'failed' : 'completed',
        error: failed ? 'Cloudflare API 返回 403：令牌缺少 Zone:Read 权限' : '',
        calls: [
          httpCall('cloudflare.verify_token', {
            origin: cloudflareApi,
            path: '/client/v4/user/tokens/verify',
            duration: 141,
            request_headers: cloudflareAuth,
            provider_request_id: '8d2c4f1a9b7ec1-HKG',
            response_headers: [
              tracePair('content-type', 'application/json'),
              cfRay('c1'),
            ],
            response_body: traceBody(
              pretty({
                result: { id: redacted, status: 'active' },
                success: true,
              })
            ),
          }),
          listZones,
        ],
        logs: failed ? [] : [[390, 'info', '可见托管域 3 个']],
      }),
      failed
        ? traceStep('record_failure', '记录失败', 5, {
            logs: [[4, 'error', '凭据权限不足，供应商已标记为无效']],
          })
        : traceStep('save_result', '保存检测结果', 9),
    ],
  }
}
function zeroSslVerificationTrace() {
  return {
    kind: 'provider',
    worker_type: 'provider-verification',
    steps: [
      traceStep('load_provider', '读取供应商状态', 5),
      traceStep('load_acme_settings', '解析证书供应商配置', 0),
      traceStep('load_eab', '获取 EAB 凭据', 1302, {
        calls: [
          httpCall('zerossl_api.eab_by_email', {
            method: 'POST',
            origin: 'https://api.zerossl.com',
            path: '/acme/eab-credentials-email',
            duration: 1296,
            request_headers: requestHeaders(
              tracePair('content-type', 'application/x-www-form-urlencoded')
            ),
            request_body: traceBody(`email=${redacted}`, {
              format: 'form',
              content_type: 'application/x-www-form-urlencoded',
              bytes: 27,
            }),
            response_headers: [tracePair('content-type', 'application/json')],
            response_body: traceBody('', {
              format: 'omitted',
              bytes: 214,
              omitted_reason: 'secret_material',
            }),
          }),
        ],
        logs: [[1300, 'info', '已获取新的 EAB 凭据']],
      }),
      traceStep('check_acme_directory', '检测 ACME 目录', 880, {
        calls: [
          httpCall('acme.directory', {
            origin: 'https://acme.zerossl.com',
            path: '/v2/DV90',
            duration: 874,
            response_headers: [tracePair('content-type', 'application/json')],
            response_body: traceBody(
              pretty({
                meta: {
                  externalAccountRequired: true,
                  termsOfService:
                    'https://secure.trust-provider.com/repository/docs/Legacy/Agreements/ZeroSSL_Terms.pdf',
                },
                newAccount: 'https://acme.zerossl.com/v2/DV90/newAccount',
                newNonce: 'https://acme.zerossl.com/v2/DV90/newNonce',
                newOrder: 'https://acme.zerossl.com/v2/DV90/newOrder',
              })
            ),
          }),
        ],
      }),
      traceStep('save_result', '保存检测结果', 11),
    ],
  }
}
function nodeApplyTrace(attempt, phase, error = '') {
  const phases = [
    ['stage', 'stage_config', '暂存配置'],
    ['validate', 'validate_config', '校验配置'],
    ['activate', 'activate_config', '激活配置'],
  ]
  const failedAt = phases.findIndex(([name]) => name === phase)
  const requestId = `release-probe-${attempt}`
  return {
    kind: 'node_apply',
    worker_type: 'web',
    steps: [
      traceStep('send_desired_state', '下发期望状态', 3, {
        calls: [
          nodeMessage('sent', 'DesiredState', {
            request_id: requestId,
            bytes: 1840,
            values: [['release_id', 'rel-20261014-0007']],
          }),
        ],
      }),
      traceStep('fetch_objects', '节点拉取发布对象', 420, {
        calls: [
          nodeMessage('received', 'ObjectRequest', {
            at: 2,
            request_id: requestId,
            object_count: 12,
            bytes: 410,
          }),
          nodeMessage('sent', 'ObjectBatch', {
            at: 30,
            request_id: requestId,
            object_count: 12,
            bytes: 182_344,
            duration: 388,
          }),
        ],
      }),
      ...phases.map(([, code, label], index) =>
        traceStep(code, label, 0, {
          outcome:
            failedAt < 0 || index < failedAt
              ? 'completed'
              : index === failedAt
                ? 'failed'
                : 'skipped',
          error: index === failedAt ? error : '',
          logs: index === 0 ? [[0, 'info', '节点未上报阶段耗时']] : [],
        })
      ),
      traceStep('record_result', '记录应用结果', 12, {
        calls: [
          nodeMessage('received', 'ApplyResult', {
            request_id: requestId,
            bytes: 214,
            values: [
              ['applied', String(failedAt < 0)],
              ...(failedAt < 0
                ? []
                : [
                    ['failed_phase', phase],
                    [
                      'error_code',
                      phase === 'validate' ? 'artifact_invalid' : 'apply_failed',
                    ],
                    ['retryable', String(phase === 'activate')],
                    ['error', error],
                  ]),
              ['release_id', 'rel-20261014-0007'],
            ],
          }),
        ],
      }),
    ],
  }
}
function buildTaskHistories() {
  // backup.test sync: 137 attempts, list truncated to the newest 100. Runs of
  // identical failures collapse; a success with a diff breaks them. Only the
  // newest 20 failures keep a full trace (the retention limit).
  const backup = []
  for (let attempt = 137, start = -40; attempt >= 1; attempt -= 1) {
    const step = 137 - attempt
    if (step < 42)
      backup.push(
        historyAttempt(attempt, 'failed', {
          start,
          duration: 204 + step * 3,
          error:
            'DescribeDomains 返回 InvalidAccessKeyId.NotFound：Specified access key is not found.',
          retry: true,
          trace: step < 20 ? aliyunSyncFailureTrace() : undefined,
        })
      )
    else if (step === 42)
      backup.push(
        historyAttempt(attempt, 'completed', {
          start,
          duration: 3480,
          detail: dnsDiff,
        })
      )
    else if (step < 78)
      backup.push(
        historyAttempt(attempt, 'failed', {
          start,
          duration: 30_000,
          error:
            '请求 dnsapi.cn 超时（30 秒）\nPOST https://dnsapi.cn/Record.List\ncontext deadline exceeded (Client.Timeout exceeded while awaiting headers)',
          retry: true,
          // Only the operations finished before the failure are listed.
          detail: dnsDetail({
            deleted: [dnsRecord('legacy-cdn', 'CNAME', 'cdn.old-vendor.test')],
            unchanged_count: 0,
          }),
        })
      )
    else
      backup.push(
        historyAttempt(attempt, 'completed', {
          start,
          duration: 900 + (step % 5) * 110,
          detail: dnsDetail(),
        })
      )
    start -= step < 42 ? 16 : 300
  }
  // example.test sync: recovered after a Cloudflare connection failure at
  // find_zone; conflicts block one push, the newest detail is truncated and
  // the oldest two rows predate the upgrade (no trace).
  const example = [
    historyAttempt(5, 'completed', {
      start: -1500,
      duration: 2731,
      detail: dnsDetail({
        created: Array.from({ length: 50 }, (_, index) =>
          dnsRecord(`node-${index + 1}`, 'A', `203.0.113.${index + 31}`)
        ),
        created_count: 64,
        unchanged_count: 10,
        truncated: true,
      }),
      trace: cloudflareSyncTrace(),
    }),
    historyAttempt(4, 'completed', {
      start: -2400,
      duration: 1650,
      // Conflicts block the push; the attempt itself still completes.
      detail: dnsDetail({
        conflicts: [
          {
            name: 'www',
            type: 'A',
            line_code: 'telecom',
            local_content: '203.0.113.10',
            remote_content: '198.51.100.77',
          },
          {
            name: 'api',
            type: 'CNAME',
            line_code: 'default',
            local_content: 'edge-qa.example.test',
            remote_content: '（远端已删除）',
          },
        ],
        unchanged_count: 16,
      }),
    }),
    historyAttempt(3, 'failed', {
      start: -9000,
      duration: 5030,
      error: 'Cloudflare API 连接失败',
      retry: true,
      detail: dnsDetail({ unchanged_count: 0 }),
      trace: cloudflareSyncFailureTrace(),
    }),
    historyAttempt(2, 'failed', {
      start: -86_400,
      error: '供应商接口返回 HTTP 502',
    }),
    historyAttempt(1, 'completed', { start: -90_000 }),
  ]
  const renew = [
    ...[4, 3, 2].map((attempt, index) =>
      historyAttempt(attempt, 'failed', {
        start: -600 - index * 1800,
        duration: 120_000 + index * 4000,
        error:
          'DNS-01 验证超时：_acme-challenge.qa.example.test 的 TXT 记录在 120 秒内未在权威服务器上生效。',
        detail: certificateDetail('renew', 'dns_challenge'),
        trace: certificateRenewFailureTrace(116_010 + index * 4000),
      })
    ),
    historyAttempt(1, 'completed', {
      start: -5_184_000,
      duration: 74_500,
      detail: certificateDetail('issue', 'completed', {
        issuer: "CN=R11,O=Let's Encrypt,C=US",
        not_before: '2026-08-15T07:12:44Z',
        not_after: '2026-11-13T07:12:43Z',
        serial_number: '04A1F3C2B9D87E6510FA33C1D2E4B5A69C70',
        fingerprint_sha256:
          '3B:7E:91:0C:44:A2:5D:E8:19:6F:C0:B3:72:58:9A:DE:01:F4:6C:28:B5:93:7A:E0:4D:16:C9:8B:52:3F:A7:E1',
      }),
      trace: certificateIssueTrace(),
    }),
  ]
  const website = [
    historyAttempt(3, 'completed', {
      start: -120,
      duration: 152,
      detail: domainsDetail(
        [
          'qa.example.test',
          'www.qa.example.test',
          '*.img.qa.example.test',
          'legacy.qa.example.test',
        ].map((hostname) =>
          domainResult(hostname, 'verified', ['edge-qa.example.test'])
        )
      ),
      trace: websiteCheckTrace([
        ['qa.example.test', 'edge-qa.example.test'],
        ['www.qa.example.test', 'edge-qa.example.test'],
        ['*.img.qa.example.test', 'edge-qa.example.test'],
        ['legacy.qa.example.test', 'edge-qa.example.test'],
      ]),
    }),
    historyAttempt(2, 'failed', {
      start: -3720,
      duration: 5120,
      error: '2 个域名未指向集群接入域名',
      retry: true,
      detail: domainsDetail(
        [
          domainResult('qa.example.test', 'verified', ['edge-qa.example.test']),
          domainResult(
            'www.qa.example.test',
            'invalid',
            ['cdn.old-vendor.test'],
            'CNAME 指向 cdn.old-vendor.test，不是集群接入域名'
          ),
          domainResult(
            '*.img.qa.example.test',
            'unverified',
            [],
            '公共 DNS 查询超时'
          ),
        ],
        3
      ),
      trace: websiteCheckTrace([
        ['qa.example.test', 'edge-qa.example.test'],
        ['www.qa.example.test', 'cdn.old-vendor.test'],
        ['*.img.qa.example.test', '', 'timeout'],
      ]),
    }),
    historyAttempt(1, 'completed', { start: -86_000 }),
  ]
  const provider = [
    historyAttempt(2, 'completed', {
      start: -900,
      duration: 412,
      detail: providerDetail({
        zone_count: 3,
        zones: [
          { name: 'example.test', status: 'active' },
          { name: 'backup.test', status: 'active' },
          { name: 'staging.example.test', status: 'pending' },
        ],
      }),
      trace: cloudflareVerificationTrace(false),
    }),
    historyAttempt(1, 'failed', {
      start: -1800,
      duration: 333,
      error: 'Cloudflare API 返回 403：令牌缺少 Zone:Read 权限',
      detail: providerDetail(),
      trace: cloudflareVerificationTrace(true),
    }),
  ]
  const certificateProvider = [
    historyAttempt(1, 'completed', {
      start: -5400,
      duration: 2210,
      detail: providerDetail({
        provider: 'zerossl',
        provider_kind: 'certificate',
        directory_url: 'https://acme.zerossl.com/v2/DV90',
        external_account: 'fetched',
      }),
      trace: zeroSslVerificationTrace(),
    }),
  ]
  // Node publish: no control-plane retry schedule; the newest record is a
  // heartbeat confirmation without a start time.
  const node = [
    historyAttempt(4, 'completed', {
      start: -30,
      detail: nodeApply({ source: 'heartbeat' }),
    }),
    historyAttempt(3, 'failed', {
      start: -400,
      duration: 2300,
      error: 'nginx 配置校验失败：unknown directive "proxy_cache_lock_age"',
      detail: nodeApply({
        result: 'failed',
        phase: 'validate',
        error_code: 'artifact_invalid',
      }),
      trace: nodeApplyTrace(
        3,
        'validate',
        'nginx 配置校验失败：unknown directive "proxy_cache_lock_age"'
      ),
    }),
    historyAttempt(2, 'failed', {
      start: -900,
      duration: 15_400,
      error: '激活配置失败：reload 超时',
      detail: nodeApply({
        result: 'failed',
        phase: 'activate',
        error_code: 'apply_failed',
        retryable: true,
      }),
      trace: nodeApplyTrace(2, 'activate', '激活配置失败：reload 超时'),
    }),
    historyAttempt(1, 'completed', {
      start: -7200,
      duration: 1830,
      detail: nodeApply(),
      trace: nodeApplyTrace(1, ''),
    }),
  ]
  const histories = {
    'dns-zone:sync:qa': backup,
    'dns-zone:sync:example': example,
    'certificate:renew:qa': renew,
    'website:deploy:qa': website,
    'provider:verify:cloudflare': provider,
    'provider:verify:zerossl': certificateProvider,
    'node:deploy:qa-06': node,
  }
  return Object.fromEntries(
    db.tasks
      .filter((task) => histories[task.id])
      .map((task) => [historyKey(task), histories[task.id]])
  )
}
db.tasks.push({
  id: 'provider:verify:zerossl',
  resource_type: 'provider',
  resource_id: certificateProviderId,
  name: 'zerossl',
  operation: 'verify',
  version: 1,
  status: 'completed',
  error: '',
  failures: 0,
  updated_at: '2026-10-01T00:20:00Z',
  next_attempt_at: '',
})
db.taskHistories = buildTaskHistories()
// The retrying sync's next attempt matches its newest history record.
{
  const task = find(db.tasks, 'dns-zone:sync:qa')
  const newest = db.taskHistories[historyKey(task)][0]
  Object.assign(task, {
    updated_at: newest.emitted_at,
    next_attempt_at: newest.next_attempt_at,
  })
}
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
        (domain) => owningZone(domain.hostname)?.id === zone.id
      )
    ).length
  }
  for (const certificate of db.certificates)
    certificate.website_count = db.websites.filter((value) =>
      value.config.certificate_ids.includes(certificate.id)
    ).length
}
// Backend `domain_belongs_to_zone` + longest match: the zone that receives a
// managed website domain's CNAME (the apex included).
function owningZone(hostname) {
  let host = hostname.trim().toLowerCase().replace(/\.+$/, '')
  if (host.startsWith('*.')) host = host.slice(2)
  let owner
  for (const zone of db.zones)
    if (
      (host === zone.domain || host.endsWith(`.${zone.domain}`)) &&
      zone.domain.length > (owner?.domain.length ?? 0)
    )
      owner = zone
  return owner
}
const websiteHostnamePattern =
  /^(?:\*\.)?([a-zA-Z0-9](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?\.)+[a-zA-Z]{2,63}$/
function validateWebsiteDomains(config) {
  for (const domain of config?.domains ?? []) {
    const hostname = domain.hostname ?? ''
    if (hostname.length > 253 || !websiteHostnamePattern.test(hostname))
      fail('域名格式不正确', 422)
    if (domain.dns_mode === 'managed' && !owningZone(hostname))
      fail('该域名不属于当前租户的托管域名', 422, 16611)
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
      // Backend: registered + enabled + heartbeat within 15 s; the mock keeps
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
    // 15 console days (UTC+8 midnights) ending with today's partial day.
    daily: Array.from({ length: 15 }, (_, index) => {
      const dayMs = 86400000
      const today =
        Math.floor((Date.parse(initialTime) + 8 * 3600000) / dayMs) * dayMs -
        8 * 3600000
      // Today's point matches the summary (1,296 requests, 144 MB so far).
      if (index === 14)
        return {
          timestamp: new Date(today).toISOString(),
          request_count: 1296,
          response_bytes: 150994944,
          bandwidth_bps: 3145728,
        }
      return {
        timestamp: new Date(today - (14 - index) * dayMs).toISOString(),
        request_count: 9000 + ((index * 3779) % 5000),
        response_bytes: 805306368 + ((index * 167772160) % 671088640),
        bandwidth_bps: 2097152 + ((index * 524288) % 4194304),
      }
    }),
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
    // Publish ids are shared by every node; resource_id selects one row.
    const id = decodeURIComponent(part[2]),
      resourceId = query.get('resource_id')
    if (resourceId !== null && !/^[0-9a-f-]{36}$/.test(resourceId))
      fail('resource_id 不合法', 400, 10001)
    const task =
      db.tasks.find(
        (item) =>
          item.id === id && (!resourceId || item.resource_id === resourceId)
      ) ?? fail('任务不存在', 404, 16701)
    if (part[3] === 'attempts' && part[5] === 'trace') {
      // One attempt across every resource of the task (publish ids are
      // shared); an attempt of another task reads as not found.
      if (!/^[1-9]\d*$/.test(part[4])) fail('attempt_id 不合法', 400, 10001)
      if (taskTraceMode === 'error')
        fail('QA 场景：执行详情读取失败', 500, 50001)
      for (const owner of db.tasks.filter((item) => item.id === id)) {
        const history = db.taskHistories[historyKey(owner)]
        const item = Array.isArray(history)
          ? history.find((entry) => entry.trace.attempt_id === part[4])
          : undefined
        if (item) return traceSnapshot(owner, item)
      }
      fail('任务不存在', 404, 16701)
    }
    if (part[3] !== 'history') return task
    const history = db.taskHistories[historyKey(task)] ?? []
    if (history === 'error') fail('QA 场景：执行记录读取失败', 500, 50001)
    return historySnapshot(task, history)
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
    taskTraceMode = 'reset'
    for (const stream of targets)
      if (!/\/(?:logs|access-logs)\/stream$/.test(stream.url.pathname))
        emitSnapshot(stream, true)
    return { reset: true }
  }
  if (event === 'task-history') {
    // History is a one-shot snapshot: the next read (tab open or 刷新记录)
    // returns the new state; only `fail` pushes the changed task streams.
    const task = find(db.tasks, query.get('id') ?? 'dns-zone:sync:qa')
    const key = historyKey(task),
      mode = query.get('mode') ?? 'reset'
    if (mode === 'fail') {
      // A new failed attempt right now that repeats the newest error (joins
      // its collapsed run) and schedules the sync retry 15 s later.
      const history = Array.isArray(db.taskHistories[key])
        ? db.taskHistories[key]
        : []
      const now = Date.now()
      const error = history[0]?.error || 'QA 场景：同步失败'
      history.unshift({
        attempt: (history[0]?.attempt ?? 0) + 1,
        outcome: 'failed',
        error,
        started_at: historyStamp(now - 1200),
        emitted_at: historyStamp(now),
        duration_ms: 1200,
        next_attempt_at: historyStamp(now + 15_000),
        detail: history[0]?.detail ?? { kind: 'none', truncated: false },
        trace: traceNone(String(++attemptSequence)),
      })
      db.taskHistories[key] = history
      Object.assign(task, {
        status: 'retrying',
        error,
        failures: task.failures + 1,
        updated_at: history[0].emitted_at,
        next_attempt_at: history[0].next_attempt_at,
      })
      broadcast(['tasks'])
    } else
      db.taskHistories[key] =
        mode === 'error'
          ? 'error'
          : mode === 'empty'
            ? []
            : structuredClone(seed.taskHistories[key] ?? [])
    return { id: task.id, mode }
  }
  if (event === 'task-trace') {
    // Traces are one-shot snapshots: `error` fails the next trace reads
    // (expand an attempt or 重试), `reset` restores them.
    const mode = query.get('mode') ?? 'reset'
    taskTraceMode = mode === 'error' ? 'error' : 'reset'
    return { mode: taskTraceMode }
  }
  if (event === 'node-runtime') {
    const node = find(db.nodes, query.get('id') ?? nodeId)
    node.runtime.last_heartbeat_at = nextTime()
    node.runtime.cpu_usage = Math.min(
      1,
      Number(query.get('cpu') ?? (node.runtime.cpu_usage ?? 0) + 0.01)
    )
    // `ips=<n>` sets the connected IP count; `ips=` drops it (a 0.4.0 node).
    const ips = query.get('ips')
    if (ips === '') delete node.runtime.connected_ip_count
    else if (ips !== null) node.runtime.connected_ip_count = Number(ips)
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
    return {
      node_id: node.id,
      cpu_usage: node.runtime.cpu_usage,
      connected_ip_count: node.runtime.connected_ip_count,
    }
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
  if (event === 'patch') {
    // Runtime/state patch without a revision bump, e.g. to cycle status
    // values: resource=websites&path=runtime.deploy_status&value=partial.
    // `value` is parsed as JSON when possible, else used as a string.
    const resource = query.get('resource') ?? 'nodes'
    const collections = {
      nodes: db.nodes,
      clusters: db.clusters,
      websites: db.websites,
      'dns-zones': db.zones,
      certificates: db.certificates,
      tasks: db.tasks,
    }
    const collection = collections[resource]
    if (!collection) fail('QA resource 不支持 patch')
    const item = find(collection, query.get('id') ?? collection[0]?.id),
      keys = (query.get('path') ?? '').split('.').filter(Boolean)
    if (keys.length === 0) fail('QA patch 需要 path')
    const raw = query.get('value') ?? ''
    let value = raw
    try {
      value = JSON.parse(raw)
    } catch {
      // Plain strings such as status names.
    }
    let target = item
    for (const key of keys.slice(0, -1)) target = target[key] ??= {}
    if (query.get('delete') === '1') delete target[keys.at(-1)]
    else target[keys.at(-1)] = value
    refreshRelations()
    broadcast([resource])
    return { id: item.id, path: keys.join('.'), value }
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
    validateWebsiteDomains(input.config)
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
      validateWebsiteDomains(input.config)
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
