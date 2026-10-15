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
// Aliyun-sized line list (several hundred lines: ISP × province, regions,
// overseas countries) and large zones for records-editor performance QA.
const aliyunIsps = [
  ['telecom', '电信'],
  ['unicom', '联通'],
  ['mobile', '移动'],
  ['edu', '教育网'],
  ['drpeng', '鹏博士'],
  ['btvn', '广电网'],
  ['cstnet', '科技网'],
  ['aliyun', '阿里云'],
]
const aliyunProvinces = (
  '北京 天津 河北 山西 内蒙古 辽宁 吉林 黑龙江 上海 江苏 浙江 安徽 福建 江西 ' +
  '山东 河南 湖北 湖南 广东 广西 海南 重庆 四川 贵州 云南 西藏 陕西 甘肃 青海 ' +
  '宁夏 新疆 香港 澳门 台湾'
).split(' ')
const aliyunCountries = {
  亚洲: '日本 韩国 新加坡 马来西亚 泰国 越南 菲律宾 印度尼西亚 印度 巴基斯坦 孟加拉 斯里兰卡 尼泊尔 缅甸 柬埔寨 老挝 蒙古 哈萨克斯坦 乌兹别克斯坦 阿联酋 沙特阿拉伯 以色列 土耳其 伊朗 伊拉克 卡塔尔 科威特 阿曼 约旦 黎巴嫩'.split(
    ' '
  ),
  欧洲: '英国 法国 德国 意大利 西班牙 葡萄牙 荷兰 比利时 瑞士 奥地利 瑞典 挪威 丹麦 芬兰 冰岛 爱尔兰 波兰 捷克 斯洛伐克 匈牙利 罗马尼亚 保加利亚 希腊 乌克兰 俄罗斯 白俄罗斯 立陶宛 拉脱维亚 爱沙尼亚 塞尔维亚 克罗地亚 斯洛文尼亚'.split(
    ' '
  ),
  北美洲: '美国 加拿大 墨西哥 古巴 牙买加 巴拿马 哥斯达黎加 危地马拉 洪都拉斯 多米尼加'.split(
    ' '
  ),
  南美洲: '巴西 阿根廷 智利 秘鲁 哥伦比亚 委内瑞拉 厄瓜多尔 乌拉圭 巴拉圭 玻利维亚'.split(
    ' '
  ),
  非洲: '埃及 南非 尼日利亚 肯尼亚 埃塞俄比亚 摩洛哥 阿尔及利亚 突尼斯 加纳 坦桑尼亚 安哥拉 苏丹'.split(
    ' '
  ),
  大洋洲: '澳大利亚 新西兰 斐济 巴布亚新几内亚'.split(' '),
}
const aliyunLines = [
  structuredClone(lines[0]),
  ...aliyunIsps.flatMap(([code, name]) => [
    { code, name, display_name: `中国地区_${name}`, status: 'enabled' },
    ...aliyunProvinces.map((province, index) => ({
      code: `cn_${code}_${index}`,
      name: `${name}_${province}`,
      display_name: `中国地区_${name}_${province}`,
      status: 'enabled',
    })),
  ]),
  ...aliyunProvinces.map((province, index) => ({
    code: `cn_region_${index}`,
    name: province,
    display_name: `中国地区_${province}`,
    status: 'enabled',
  })),
  { code: 'oversea', name: '境外', display_name: '境外', status: 'enabled' },
  ...Object.entries(aliyunCountries).flatMap(([continent, countries], c) => [
    {
      code: `os_${c}`,
      name: continent,
      display_name: `境外_${continent}`,
      status: 'enabled',
    },
    ...countries.map((country, index) => ({
      code: `os_${c}_${index}`,
      name: country,
      display_name: `境外_${continent}_${country}`,
      status: 'enabled',
    })),
  ]),
  ...['百度', '谷歌', '必应', '搜狗', '360', '有道'].map((engine, index) => ({
    code: `search_${index}`,
    name: engine,
    display_name: `搜索引擎_${engine}`,
    status: index < 5 ? 'enabled' : 'disabled',
  })),
]
const bulkRecordTypes = ['A', 'A', 'A', 'AAAA', 'CNAME', 'TXT', 'MX']
function bulkRecords(first, count, lineCodes) {
  return Array.from({ length: count }, (_, index) => {
    const type = bulkRecordTypes[index % bulkRecordTypes.length]
    return {
      id: uuid(first + index),
      type,
      name: `host-${String(index + 1).padStart(3, '0')}`,
      content:
        type === 'A'
          ? `198.51.100.${(index % 250) + 1}`
          : type === 'AAAA'
            ? `2001:db8::${(index + 1).toString(16)}`
            : type === 'TXT'
              ? `v=spf1 include:_spf.bulk.test ~all ${index}`
              : `edge-${index}.bulk.test`,
      ttl: 600,
      ...(type === 'MX' ? { priority: 10 } : {}),
      proxied: false,
      line_code: lineCodes[(index * 7) % lineCodes.length],
    }
  })
}
function bulkZone({ id, domain, providerId, provider, providerName, records, zoneLines }) {
  return {
    id,
    revision: 2,
    dns_provider_id: providerId,
    dns_provider: provider,
    dns_provider_name: providerName,
    domain,
    sync_status: 'synced',
    desired_revision: 2,
    synced_revision: 2,
    website_count: 0,
    config: { records },
    runtime: {
      records_imported: true,
      lines_synced_at: initialTime,
      lines: structuredClone(zoneLines),
      projected_records: [],
      record_states: records.map((record) => ({
        id: record.id,
        sync_status: 'synced',
        synced_revision: 2,
      })),
      conflicts: [],
    },
    last_synced_at: initialTime,
    ...timestamps,
  }
}
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
    // Records-editor performance QA: 300 records on an Aliyun-sized line list
    // and 50 Cloudflare records (default line only).
    bulkZone({
      id: uuid(20000),
      domain: 'bulk-aliyun.test',
      providerId: uuid(802),
      provider: 'aliyun',
      providerName: '阿里云备用账号',
      records: bulkRecords(
        20001,
        300,
        aliyunLines.map(({ code }) => code)
      ),
      zoneLines: aliyunLines,
    }),
    bulkZone({
      id: uuid(21000),
      domain: 'bulk-cloudflare.test',
      providerId: dnsProviderId,
      provider: 'cloudflare',
      providerName: 'Cloudflare 生产账号',
      records: bulkRecords(21001, 50, ['default']),
      zoneLines: lines,
    }),
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
// Task center (build/operation-tasks/contract.md): a task is one user
// operation or system action (`db.operations`), its subtasks the background
// work it caused (embedded as `subtasks`). Execution records live in
// `db.subtaskRecords[subtask id]`, newest first, outside the tasks so list
// keyword search ignores them. A task's status, counters and times are
// derived on read (design §5.2). Times use the backend's `…ffffff+00` format,
// relative to server start so "n 秒后重试" counts down in the browser.
// Streams: `/api/operations/stream` (list), `/api/operations/:id/stream
// ?subtask_id=` (`snapshot`, then `history` of the expanded subtask; a new
// record arrives as `attempt` before the new `snapshot`) and
// `/api/operations/:id/attempts/:attempt_id/trace/stream` (one-shot trace).
// Writes that start background work create a task and answer its
// `operation_id` (contract §6). QA controls (`/__qa/control?event=…`):
// - `operation-attempt&operation=<id>&subtask=<id>&outcome=failed|completed`
//   appends a record now (failed → retrying, retry 15 s later;
//   `retryable=0` makes a node failure final);
// - `operation-complete&operation=<id>&subtask=<id>` completes a subtask;
// - `operation-claim&operation=<id>&subtask=<id>` runs sync-type work (node
//   work is never `running`);
// - `operation-create&action=website.update&target=<resource id>`;
// - `operation-history-reset&operation=<id>&subtask=<id>&keep=0` drops all
//   but the newest `keep` records (retention) and re-sends `history`;
// - `operation-trace&mode=error|reset` fails every trace read.
const historyBase = Date.now()
const historyStamp = (ms) => new Date(ms).toISOString().replace('Z', '000+00')
const historyAt = (seconds) => historyStamp(historyBase + seconds * 1000)
const stampMs = (stamp) =>
  Date.parse(stamp.replace(/(\.\d{3})\d{3}\+00$/, '$1Z'))
// Full traces by attempt id; immutable like stored traces, so outside `db`.
const traceDocuments = new Map()
let attemptSequence = 48000
let operationTraceMode = 'reset'
const user = {
  id: uuid(101),
  username: 'admin',
  nickname: 'QA 管理员',
  status: 'active',
}
const operationStatuses = [
  'queued',
  'running',
  'retrying',
  'failed',
  'partial_failed',
  'attention_required',
  'completed',
  'merged',
  'cancelled',
]
const operationGroups = {
  active: ['queued', 'running', 'retrying'],
  attention: ['retrying', 'failed', 'partial_failed', 'attention_required'],
}
const settledStatuses = [
  'completed',
  'failed',
  'attention_required',
  'merged',
  'cancelled',
]
const operationTargetTypes = [
  'website',
  'cluster',
  'node',
  'dns_zone',
  'certificate',
  'provider',
]
function historyAttempt(
  attempt,
  outcome,
  {
    start,
    duration,
    error = '',
    retry = false,
    retryAt,
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
    // Sync work retries 15 s after a failure; `retryAt` (seconds from server
    // start) pins the next attempt instead.
    next_attempt_at:
      retryAt !== undefined
        ? historyAt(retryAt)
        : retry
          ? historyAt(emitted + 15)
          : '',
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
/** Full trace snapshot: attempt fields from the record, steps stored. */
function traceSnapshot(subtask, item) {
  const stored = traceDocuments.get(item.trace.attempt_id)
  return {
    attempt_id: item.trace.attempt_id,
    subtask_id: subtask.id,
    resource_type: subtask.resource_type,
    resource_id: subtask.resource_id,
    kind: stored?.kind ?? item.detail.kind,
    state: stored ? 'available' : 'none',
    attempt: item.attempt,
    outcome: item.outcome,
    error: item.error,
    started_at: item.started_at,
    emitted_at: item.emitted_at,
    duration_ms: item.duration_ms,
    worker_type:
      stored?.worker_type ?? workerTypes[subtask.resource_type] ?? '',
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
/** `history_summary_dto` over every retained record of a subtask. */
function historySummary(subtask) {
  const records = db.subtaskRecords[subtask.id] ?? []
  const failures = records.filter((item) => item.outcome === 'failed')
  return {
    attempt_count: records.length,
    failure_count: failures.length,
    first_failure_at: failures.at(-1)?.emitted_at ?? '',
    last_success_at:
      records.find((item) => item.outcome === 'completed')?.emitted_at ?? '',
    next_attempt_at:
      subtask.status === 'retrying' ? subtask.next_attempt_at : '',
  }
}
/** `subtask_history_dto`: the newest 100 records. */
function subtaskHistory(subtask) {
  const records = db.subtaskRecords[subtask.id] ?? []
  return {
    subtask_id: subtask.id,
    list: records.slice(0, 100),
    truncated: records.length > 100,
    summary: historySummary(subtask),
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
  domain_count: domains.length,
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
      traceStep('find_zone', '查找远程托管域名', 5012, {
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
      traceStep('find_zone', '查找远程托管域名', 212, {
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
      traceStep('find_zone', '查找远程托管域名', 186, {
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
function acmeAuthorization(index, status, domain, { at = 1, repeat = 1 } = {}) {
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
// Trace link from a certificate DNS step (contract §3.7): `subtask_id` is
// the matching child subtask of the task, or '' when it has none.
const relatedLabels = {
  dns_challenge: 'DNS 验证记录',
  dns_challenge_cleanup: '清理验证记录',
}
const relatedDns = (operationId, kind, zone, subtask) => ({
  task_id: operationId,
  resource_type: 'dns_zone',
  resource_id: zone.id,
  label: `${relatedLabels[kind]} · ${zone.domain}`,
  subtask_id: subtask?.id ?? '',
})
function challengeCheck(at, repeat, data, domain) {
  const name = `_acme-challenge.${domain}`
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
const certificatePrepare = (operation, domains) =>
  traceStep('prepare', '准备签发', 21, {
    logs: [
      [
        5,
        'info',
        `${operation === 'renew' ? '续期' : '签发'}证书：${domains.join('、')}`,
      ],
    ],
  })
const certificateOrder = (domains) =>
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
          identifiers: domains.map((value) => ({ type: 'dns', value })),
        }),
        response_headers: [
          tracePair('location', `${letsEncrypt}/acme/order/1789320/30419875`),
          ...nonceHeaders(),
        ],
        response_body: traceBody(
          pretty({
            authorizations: domains.map(
              (_, index) => `${letsEncrypt}${authzPath(index)}`
            ),
            finalize: `${letsEncrypt}/acme/finalize/1789320/30419875`,
            status: 'pending',
          })
        ),
      }),
    ],
  })
const certificateAuthorization = (domains) =>
  traceStep('authorization', '获取域名授权', 210, {
    calls: domains.map((domain, index) =>
      acmeAuthorization(index, 'pending', domain, { at: 1 + index * 103 })
    ),
  })
/** `challenge`/`cleanup`: `related_task` of the DNS steps, if any. */
function certificateRenewFailureTrace({ wait, domains, challenge, cleanup }) {
  return {
    kind: 'certificate',
    worker_type: 'certificate',
    steps: [
      certificatePrepare('renew', domains),
      traceStep('account', 'ACME 账户', 190, {
        calls: [acmeDirectory(1), acmeNonce(150)],
        logs: [[188, 'info', '复用已注册的 ACME 账户']],
      }),
      certificateOrder(domains),
      certificateAuthorization(domains),
      traceStep('dns_challenge', '写入 DNS 验证记录并等待生效', wait, {
        subject: `_acme-challenge.${domains[0]}`,
        outcome: 'failed',
        error: `DNS-01 验证超时：_acme-challenge.${domains[0]} 的 TXT 记录在 120 秒内未在权威服务器上生效。`,
        related_task: challenge,
        calls: [challengeCheck(10_200, 14, `"${redacted}"`, domains[0])],
        logs: [
          [8, 'info', `已写入 ${domains.length} 条验证记录，等待 DNS 同步任务完成`],
          [10_100, 'info', 'DNS 同步已完成，开始检查公共解析'],
          [18_300, 'warning', 'TXT 记录尚未生效：解析结果与期望值不一致', 14],
          [wait - 4, 'error', '120 秒内 TXT 记录未生效，停止等待'],
        ],
      }),
      traceStep('cleanup', '清理 DNS 验证记录', 3100, {
        related_task: cleanup,
        logs: [[3096, 'info', `已删除 ${domains.length} 条验证记录`]],
      }),
      traceStep('record_failure', '记录失败', 6, {
        logs: [[5, 'error', '续期失败，将在下一个续期窗口重试']],
      }),
    ],
  }
}
function certificateIssueTrace({ domains, challenge, cleanup }) {
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
      certificatePrepare('issue', domains),
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
      certificateOrder(domains),
      certificateAuthorization(domains),
      traceStep('dns_challenge', '写入 DNS 验证记录并等待生效', 31_000, {
        subject: `_acme-challenge.${domains[0]}`,
        related_task: challenge,
        calls: [challengeCheck(10_400, 3, `"${redacted}"`, domains[0])],
        logs: [
          [8, 'info', `已写入 ${domains.length} 条验证记录，等待 DNS 同步任务完成`],
          [20_900, 'info', 'TXT 记录已在公共解析生效，固定等待 10 秒'],
        ],
      }),
      traceStep('validation', '证书颁发机构验证', 22_000, {
        calls: [
          ...domains.map((_, index) =>
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
          acmeAuthorization(0, 'pending', domains[0], { at: 300, repeat: 9 }),
          ...domains.map((domain, index) =>
            acmeAuthorization(index, 'valid', domain, { at: 18_400 + index * 200 })
          ),
        ],
        logs: [
          [300, 'info', '授权状态 pending，2 秒后再次检查', 9],
          [18_700, 'info', '全部授权已生效'],
        ],
      }),
      traceStep('cleanup', '清理 DNS 验证记录', 2800, {
        related_task: cleanup,
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
                  subject: `CN=${domains[0]}`,
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
// Task fixtures. Together they cover every task status (merged, cancelled,
// partial failure and a system task included, plus a task by another
// administrator for `actor=me`), every subtask kind but `dns_delete`,
// recovered and >100-record (truncated) histories, a certificate issue whose
// DNS steps link to its child subtasks, and a record without start time or
// trace (heartbeat confirmation). IDs: tasks uuid(7001…), their subtasks
// uuid(7100 + 10 × task + n).
db.subtaskRecords = {}
const operatorId = uuid(102)
/** One stored subtask; its records (newest first) go to `db.subtaskRecords`.
 * Settlement time, next retry and error default to the newest record. */
function seedSubtask(number, fields, records = []) {
  const id = uuid(number)
  db.subtaskRecords[id] = records
  return {
    id,
    parent_id: '',
    warning: '',
    running_since: '',
    finished_at: settledStatuses.includes(fields.status)
      ? (records[0]?.emitted_at ?? fields.created_at)
      : '',
    next_attempt_at:
      fields.status === 'retrying' ? (records[0]?.next_attempt_at ?? '') : '',
    error: ['retrying', 'failed'].includes(fields.status)
      ? (records[0]?.error ?? '')
      : '',
    ...fields,
  }
}
const seedOperation = (number, fields, subtasks) => ({
  id: uuid(number),
  actor_type: 'user',
  actor_user_id: user.id,
  actor_name: user.nickname,
  summary: {},
  ...fields,
  subtasks,
})
const work = (kind, resource_type, resource_id, resource_name) => ({
  kind,
  resource_type,
  resource_id,
  resource_name,
})
const nodeResult = (node, name, patch) =>
  nodeApply({ node_id: node, node_name: name, ...patch })
const reloadTimeout = '激活配置失败：reload 超时'
const nginxInvalid =
  'nginx 配置校验失败：unknown directive "proxy_cache_lock_age"'
const shopDomains = ['shop.example.test'],
  legacyDomains = ['legacy.example.test'],
  removedNodeId = uuid(7090),
  removedProviderId = uuid(7091)
// backup.test sync: 137 records, the list truncated to the newest 100. Runs
// of identical failures collapse; a success with a diff breaks them. Only the
// newest 20 failures keep a full trace (the retention limit).
const backupRecords = []
for (let attempt = 137, start = -10; attempt >= 1; attempt -= 1) {
  const step = 137 - attempt
  if (step < 42)
    backupRecords.push(
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
    backupRecords.push(
      historyAttempt(attempt, 'completed', {
        start,
        duration: 3480,
        detail: dnsDetail({
          created: [
            dnsRecord('static', 'CNAME', 'edge-qa.example.test'),
            dnsRecord('_acme-challenge.static', 'TXT', 'Zq8m1y2QvB7oP0xKk3sNw4rT5uV6', {
              ttl: 120,
            }),
          ],
          updated: [
            {
              before: dnsRecord('www', 'A', '203.0.113.10', {
                line_code: 'telecom',
              }),
              after: dnsRecord('www', 'A', '203.0.113.24', {
                ttl: 300,
                line_code: 'telecom',
              }),
            },
            {
              before: dnsRecord('@', 'MX', 'mx1.backup.test', { priority: 10 }),
              after: dnsRecord('@', 'MX', 'mx2.backup.test', { priority: 5 }),
            },
          ],
          deleted: [dnsRecord('old-api', 'AAAA', '2001:db8::17')],
          unchanged_count: 14,
        }),
      })
    )
  else if (step < 78)
    backupRecords.push(
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
    backupRecords.push(
      historyAttempt(attempt, 'completed', {
        start,
        duration: 900 + (step % 5) * 110,
        detail: dnsDetail(),
      })
    )
  start -= step < 42 ? 16 : 300
}
const backupCreatedAt = historyStamp(
  stampMs(backupRecords.at(-1).started_at) - 2000
)
const challengeRecord = (name, token) =>
  dnsRecord(`_acme-challenge.${name}`, 'TXT', token, { ttl: 120 })
// Certificate DNS steps link to these child subtasks (contract §3.7).
const shopChallenge = seedSubtask(
    7122,
    {
      ...work('dns_challenge', 'dns_zone', zoneId, 'example.test'),
      parent_id: uuid(7121),
      status: 'completed',
      created_at: historyAt(-10_793),
    },
    [
      historyAttempt(1, 'completed', {
        start: -10_793,
        duration: 1210,
        detail: dnsDetail({
          created: [challengeRecord('shop', 'Zq8m1y2QvB7oP0xKk3sNw4rT5uV6')],
        }),
      }),
    ]
  ),
  shopCleanup = seedSubtask(
    7123,
    {
      ...work('dns_challenge_cleanup', 'dns_zone', zoneId, 'example.test'),
      parent_id: uuid(7121),
      status: 'completed',
      created_at: historyAt(-10_740),
    },
    [
      historyAttempt(1, 'completed', {
        start: -10_740,
        duration: 980,
        detail: dnsDetail({
          deleted: [challengeRecord('shop', 'Zq8m1y2QvB7oP0xKk3sNw4rT5uV6')],
        }),
      }),
    ]
  ),
  // Every renewal attempt writes and then removes the TXT records again.
  renewChallenge = seedSubtask(
    7182,
    {
      ...work('dns_challenge', 'dns_zone', zoneId, 'example.test'),
      parent_id: uuid(7181),
      status: 'completed',
      created_at: historyAt(-398),
    },
    [-128, -263, -398].map((start, index) =>
      historyAttempt(3 - index, 'completed', {
        start,
        duration: 1180,
        detail: dnsDetail({
          created: [challengeRecord('legacy', 'Hn4c7Lq0WzR2yT8vB1mK5sXe9PdA')],
        }),
      })
    )
  ),
  renewCleanup = seedSubtask(
    7183,
    {
      ...work('dns_challenge_cleanup', 'dns_zone', zoneId, 'example.test'),
      parent_id: uuid(7181),
      status: 'completed',
      created_at: historyAt(-275),
    },
    [-13, -144, -275].map((start, index) =>
      historyAttempt(3 - index, 'completed', {
        start,
        duration: 1040,
        detail: dnsDetail({
          deleted: [challengeRecord('legacy', 'Hn4c7Lq0WzR2yT8vB1mK5sXe9PdA')],
        }),
      })
    )
  )
const exampleZone = { id: zoneId, domain: 'example.test' }
db.operations = [
  // Retrying: QA Edge 02 fails to activate (next attempt 40 s after start);
  // the DNS sync recovered after one failure.
  seedOperation(
    7001,
    {
      action: 'website.update',
      target_type: 'website',
      target_id: uuid(1201),
      target_name: '官网',
      created_at: historyAt(-900),
      summary: { sections: ['domains', 'https'] },
    },
    [
      seedSubtask(
        7111,
        {
          ...work('dns_sync', 'dns_zone', zoneId, 'example.test'),
          status: 'completed',
          created_at: historyAt(-900),
        },
        [
          historyAttempt(2, 'completed', {
            start: -878,
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
          historyAttempt(1, 'failed', {
            start: -898,
            duration: 5030,
            error: 'Cloudflare API 连接失败',
            retry: true,
            detail: dnsDetail({ unchanged_count: 0 }),
            trace: cloudflareSyncFailureTrace(),
          }),
        ]
      ),
      seedSubtask(
        7112,
        {
          ...work('node_publish', 'node', nodeId, 'QA Edge 01'),
          status: 'completed',
          created_at: historyAt(-900),
        },
        [
          historyAttempt(1, 'completed', {
            start: -870,
            duration: 1830,
            detail: nodeResult(nodeId, 'QA Edge 01'),
            trace: nodeApplyTrace(1, ''),
          }),
        ]
      ),
      seedSubtask(
        7113,
        {
          ...work('node_publish', 'node', uuid(973), 'QA Edge 02'),
          status: 'retrying',
          created_at: historyAt(-900),
        },
        [
          historyAttempt(3, 'failed', {
            start: -26,
            duration: 15_400,
            error: reloadTimeout,
            retryAt: 40,
            detail: nodeResult(uuid(973), 'QA Edge 02', {
              result: 'failed',
              phase: 'activate',
              error_code: 'apply_failed',
              retryable: true,
            }),
            trace: nodeApplyTrace(3, 'activate', reloadTimeout),
          }),
          historyAttempt(2, 'failed', {
            start: -420,
            duration: 15_200,
            error: reloadTimeout,
            retry: true,
            detail: nodeResult(uuid(973), 'QA Edge 02', {
              result: 'failed',
              phase: 'activate',
              error_code: 'apply_failed',
              retryable: true,
            }),
            trace: nodeApplyTrace(2, 'activate', reloadTimeout),
          }),
          historyAttempt(1, 'failed', {
            start: -868,
            duration: 2300,
            error: '暂存配置失败：磁盘空间不足（剩余 112 MiB）',
            retry: true,
            detail: nodeResult(uuid(973), 'QA Edge 02', {
              result: 'failed',
              phase: 'stage',
              error_code: 'apply_failed',
              retryable: true,
            }),
          }),
        ]
      ),
      seedSubtask(
        7114,
        {
          ...work('website_check', 'website', uuid(1201), '官网'),
          status: 'completed',
          created_at: historyAt(-900),
        },
        [
          historyAttempt(1, 'completed', {
            start: -860,
            duration: 152,
            detail: domainsDetail(
              ['www.example.test', 'www-cn.example.test'].map((hostname) =>
                domainResult(hostname, 'verified', ['edge-qa.example.test'])
              )
            ),
            trace: websiteCheckTrace([
              ['www.example.test', 'edge-qa.example.test'],
              ['www-cn.example.test', 'edge-qa.example.test'],
            ]),
          }),
        ]
      ),
    ]
  ),
  // Completed certificate issue with its DNS child subtasks, then publish.
  seedOperation(
    7002,
    {
      action: 'certificate.create',
      target_type: 'certificate',
      target_id: uuid(993),
      target_name: 'shop.example.test',
      created_at: historyAt(-10_800),
    },
    [
      seedSubtask(
        7121,
        {
          ...work('certificate_issue', 'certificate', uuid(993), 'shop.example.test'),
          status: 'completed',
          created_at: historyAt(-10_800),
        },
        [
          historyAttempt(1, 'completed', {
            start: -10_795,
            duration: 74_500,
            detail: certificateDetail('issue', 'completed', {
              domains: shopDomains,
              issuer: "CN=R11,O=Let's Encrypt,C=US",
              not_before: '2026-07-28T00:00:00Z',
              not_after: '2026-10-26T00:00:00Z',
              serial_number: '04A1F3C2B9D87E6510FA33C1D2E4B5A69C70',
              fingerprint_sha256: Array(32).fill('CD').join(':'),
            }),
            trace: certificateIssueTrace({
              domains: shopDomains,
              challenge: relatedDns(
                uuid(7002),
                'dns_challenge',
                exampleZone,
                shopChallenge
              ),
              cleanup: relatedDns(
                uuid(7002),
                'dns_challenge_cleanup',
                exampleZone,
                shopCleanup
              ),
            }),
          }),
        ]
      ),
      shopChallenge,
      shopCleanup,
      seedSubtask(
        7124,
        {
          ...work('node_publish', 'node', nodeId, 'QA Edge 01'),
          status: 'completed',
          created_at: historyAt(-10_721),
        },
        [
          historyAttempt(1, 'completed', {
            start: -10_720,
            duration: 1650,
            detail: nodeResult(nodeId, 'QA Edge 01'),
            trace: nodeApplyTrace(1, ''),
          }),
        ]
      ),
    ]
  ),
  // Needs attention: the pushed records conflict with a remote change.
  seedOperation(
    7003,
    {
      action: 'dns_zone.update_records',
      target_type: 'dns_zone',
      target_id: uuid(987),
      target_name: 'conflict.test',
      actor_user_id: operatorId,
      actor_name: '运维值班',
      created_at: historyAt(-5400),
      summary: { record_counts: { created: 2, updated: 1, deleted: 0 } },
    },
    [
      seedSubtask(
        7131,
        {
          ...work('dns_sync', 'dns_zone', uuid(987), 'conflict.test'),
          status: 'attention_required',
          warning: 'dns_conflict',
          created_at: historyAt(-5400),
        },
        [
          historyAttempt(1, 'completed', {
            start: -5398,
            duration: 1650,
            detail: dnsDetail({
              created: [
                dnsRecord('api', 'A', '192.0.2.21'),
                dnsRecord('@', 'MX', 'mx.conflict.test', { priority: 10 }),
              ],
              updated: [
                {
                  before: dnsRecord('@', 'TXT', '"v=spf1 -all"'),
                  after: dnsRecord('@', 'TXT', '"v=spf1 mx -all"'),
                },
              ],
              conflicts: [
                {
                  name: 'www',
                  type: 'A',
                  line_code: 'default',
                  local_content: '192.0.2.10',
                  remote_content: '198.51.100.7',
                },
              ],
              unchanged_count: 3,
            }),
          }),
        ]
      ),
    ]
  ),
  // Merged: a newer save of the same website took over the DNS sync and the
  // publish; the domain check was absorbed by the check of 7012, which has
  // no receiving subtask to point to (`closed_by` only).
  seedOperation(
    7004,
    {
      action: 'website.update',
      target_type: 'website',
      target_id: brandWebsiteId,
      target_name: '品牌官网',
      created_at: historyAt(-300),
      summary: { sections: ['origins'] },
    },
    [
      seedSubtask(7141, {
        ...work('dns_sync', 'dns_zone', brandZoneId, 'brand.test'),
        status: 'merged',
        created_at: historyAt(-300),
        finished_at: historyAt(-20),
        merged_into: { operation_id: uuid(7007), subtask_id: uuid(7172) },
      }),
      seedSubtask(7142, {
        ...work('node_publish', 'node', uuid(1004), 'QA Edge 07'),
        status: 'merged',
        created_at: historyAt(-300),
        finished_at: historyAt(-20),
        merged_into: { operation_id: uuid(7007), subtask_id: uuid(7174) },
      }),
      seedSubtask(7143, {
        ...work('website_check', 'website', brandWebsiteId, '品牌官网'),
        status: 'merged',
        created_at: historyAt(-300),
        finished_at: historyAt(-178),
        closed_by: { operation_id: uuid(7012) },
      }),
    ]
  ),
  // Cancelled by the deletion of the (now removed) node.
  seedOperation(
    7005,
    {
      action: 'node.update',
      target_type: 'node',
      target_id: removedNodeId,
      target_name: '广州 Edge 02',
      created_at: historyAt(-172_800),
      summary: { sections: ['endpoints'] },
    },
    [
      seedSubtask(7151, {
        ...work('node_config', 'node', removedNodeId, '广州 Edge 02'),
        status: 'cancelled',
        created_at: historyAt(-172_800),
        finished_at: historyAt(-171_000),
        closed_by: { operation_id: uuid(7006) },
      }),
    ]
  ),
  seedOperation(
    7006,
    {
      action: 'node.delete',
      target_type: 'node',
      target_id: removedNodeId,
      target_name: '广州 Edge 02',
      created_at: historyAt(-171_000),
    },
    [
      seedSubtask(
        7161,
        {
          ...work('dns_sync', 'dns_zone', zoneId, 'example.test'),
          status: 'completed',
          created_at: historyAt(-171_000),
        },
        [
          historyAttempt(1, 'completed', {
            start: -170_998,
            duration: 1320,
            detail: dnsDetail({
              deleted: [dnsRecord('edge', 'A', '198.51.100.62')],
              unchanged_count: 17,
            }),
          }),
        ]
      ),
    ]
  ),
  // Running (newest): the domain check runs; node publishes never run (they
  // wait for the node), QA Edge 07 is offline.
  seedOperation(
    7007,
    {
      action: 'website.update',
      target_type: 'website',
      target_id: brandWebsiteId,
      target_name: '品牌官网',
      created_at: historyAt(-20),
      summary: { sections: ['domains'] },
    },
    [
      seedSubtask(
        7171,
        {
          ...work('dns_sync', 'dns_zone', cdnZoneId, 'cdn.example.test'),
          status: 'completed',
          created_at: historyAt(-20),
        },
        [
          historyAttempt(1, 'completed', {
            start: -19,
            duration: 1840,
            detail: dnsDetail({
              updated: [
                {
                  before: dnsRecord('img', 'CNAME', 'edge.example.test'),
                  after: dnsRecord('img', 'CNAME', 'edge.example.test', {
                    ttl: 300,
                  }),
                },
              ],
              unchanged_count: 0,
            }),
          }),
        ]
      ),
      seedSubtask(
        7172,
        {
          ...work('dns_sync', 'dns_zone', brandZoneId, 'brand.test'),
          status: 'attention_required',
          warning: 'dns_unpushed',
          created_at: historyAt(-20),
        },
        [
          historyAttempt(1, 'completed', {
            start: -18,
            duration: 2240,
            detail: dnsDetail({
              created: [dnsRecord('www', 'CNAME', 'edge.example.test')],
              unchanged_count: 2,
            }),
          }),
        ]
      ),
      seedSubtask(7173, {
        ...work('node_publish', 'node', nodeId, 'QA Edge 01'),
        status: 'queued',
        created_at: historyAt(-20),
      }),
      seedSubtask(7174, {
        ...work('node_publish', 'node', uuid(1004), 'QA Edge 07'),
        status: 'queued',
        created_at: historyAt(-20),
      }),
      seedSubtask(7175, {
        ...work('website_check', 'website', brandWebsiteId, '品牌官网'),
        status: 'running',
        created_at: historyAt(-20),
        running_since: historyAt(-6),
      }),
    ]
  ),
  // System task: the automatic renewal keeps retrying (sync-type work never
  // fails for good); every attempt wrote and removed the TXT records.
  seedOperation(
    7008,
    {
      action: 'system.certificate_auto_renew',
      target_type: 'certificate',
      target_id: uuid(994),
      target_name: 'legacy.example.test',
      actor_type: 'system',
      actor_user_id: '',
      actor_name: '',
      created_at: historyAt(-405),
    },
    [
      seedSubtask(
        7181,
        {
          ...work('certificate_renew', 'certificate', uuid(994), 'legacy.example.test'),
          status: 'retrying',
          created_at: historyAt(-405),
        },
        [3, 2, 1].map((attempt, index) =>
          historyAttempt(attempt, 'failed', {
            start: -130 - index * 135,
            duration: 120_000 + index * 4000,
            error:
              'DNS-01 验证超时：_acme-challenge.legacy.example.test 的 TXT 记录在 120 秒内未在权威服务器上生效。',
            retry: true,
            retryAt: index ? undefined : 50,
            detail: certificateDetail('renew', 'dns_challenge', {
              domains: legacyDomains,
            }),
            trace: certificateRenewFailureTrace({
              wait: 116_010 + index * 4000,
              domains: legacyDomains,
              challenge: relatedDns(
                uuid(7008),
                'dns_challenge',
                exampleZone,
                renewChallenge
              ),
              cleanup: relatedDns(
                uuid(7008),
                'dns_challenge_cleanup',
                exampleZone,
                renewCleanup
              ),
            }),
          })
        )
      ),
      renewChallenge,
      renewCleanup,
    ]
  ),
  // Provider check (another administrator): recovered, then one DNS sync
  // per zone of the provider as child subtasks.
  seedOperation(
    7009,
    {
      action: 'provider.verify',
      target_type: 'provider',
      target_id: dnsProviderId,
      target_name: 'Cloudflare 生产账号',
      actor_user_id: operatorId,
      actor_name: '运维值班',
      created_at: historyAt(-259_200),
    },
    [
      seedSubtask(
        7191,
        {
          ...work('provider_verify', 'provider', dnsProviderId, 'Cloudflare 生产账号'),
          status: 'completed',
          created_at: historyAt(-259_200),
        },
        [
          historyAttempt(2, 'completed', {
            start: -259_168,
            duration: 412,
            detail: providerDetail({
              zone_count: 3,
              zones: [
                { name: 'example.test', status: 'active' },
                { name: 'brand.test', status: 'active' },
                { name: 'staging.example.test', status: 'pending' },
              ],
            }),
            trace: cloudflareVerificationTrace(false),
          }),
          historyAttempt(1, 'failed', {
            start: -259_198,
            duration: 333,
            error: 'Cloudflare API 返回 403：令牌缺少 Zone:Read 权限',
            retry: true,
            detail: providerDetail(),
            trace: cloudflareVerificationTrace(true),
          }),
        ]
      ),
      ...[
        [7192, zoneId, 'example.test'],
        [7193, brandZoneId, 'brand.test'],
      ].map(([number, zone, domain], index) =>
        seedSubtask(
          number,
          {
            ...work('dns_sync', 'dns_zone', zone, domain),
            parent_id: uuid(7191),
            status: 'completed',
            created_at: historyAt(-259_167),
          },
          [
            historyAttempt(1, 'completed', {
              start: -259_166 + index,
              duration: 980,
              detail: dnsDetail(),
            }),
          ]
        )
      ),
    ]
  ),
  // Retrying for hours: 137 records, the history list truncated to 100.
  seedOperation(
    7010,
    {
      action: 'dns_zone.sync',
      target_type: 'dns_zone',
      target_id: uuid(983),
      target_name: 'backup.test',
      created_at: backupCreatedAt,
    },
    [
      seedSubtask(
        7201,
        {
          ...work('dns_sync', 'dns_zone', uuid(983), 'backup.test'),
          status: 'retrying',
          created_at: backupCreatedAt,
        },
        backupRecords
      ),
    ]
  ),
  // Partial failure: QA Edge 06 rejects the configuration (not retryable);
  // QA Edge 05 was confirmed by a heartbeat (record without start or trace).
  seedOperation(
    7011,
    {
      action: 'cluster.update',
      target_type: 'cluster',
      target_id: clusterId,
      target_name: 'QA Cluster',
      created_at: historyAt(-7200),
      summary: { sections: ['basic'] },
    },
    [
      seedSubtask(
        7211,
        {
          ...work('dns_sync', 'dns_zone', zoneId, 'example.test'),
          status: 'completed',
          created_at: historyAt(-7200),
        },
        [
          historyAttempt(1, 'completed', {
            start: -7195,
            duration: 1120,
            detail: dnsDetail({
              updated: [
                {
                  before: dnsRecord('edge', 'A', '192.0.2.10'),
                  after: dnsRecord('edge', 'A', '192.0.2.10', { ttl: 300 }),
                },
              ],
              unchanged_count: 16,
            }),
          }),
        ]
      ),
      seedSubtask(
        7212,
        {
          ...work('node_publish', 'node', uuid(1002), 'QA Edge 05'),
          status: 'completed',
          created_at: historyAt(-7200),
        },
        [
          historyAttempt(1, 'completed', {
            start: -7150,
            detail: nodeResult(uuid(1002), 'QA Edge 05', {
              source: 'heartbeat',
            }),
          }),
        ]
      ),
      seedSubtask(
        7213,
        {
          ...work('node_publish', 'node', uuid(1003), 'QA Edge 06'),
          status: 'failed',
          created_at: historyAt(-7200),
        },
        [
          historyAttempt(1, 'failed', {
            start: -7180,
            duration: 2300,
            error: nginxInvalid,
            detail: nodeResult(uuid(1003), 'QA Edge 06', {
              result: 'failed',
              phase: 'validate',
              error_code: 'artifact_invalid',
            }),
            trace: nodeApplyTrace(1, 'validate', nginxInvalid),
          }),
        ]
      ),
    ]
  ),
  // Needs attention: the apex still points to the old vendor.
  seedOperation(
    7012,
    {
      action: 'website.check_domains',
      target_type: 'website',
      target_id: brandWebsiteId,
      target_name: '品牌官网',
      created_at: historyAt(-200),
    },
    [
      seedSubtask(
        7221,
        {
          ...work('website_check', 'website', brandWebsiteId, '品牌官网'),
          status: 'attention_required',
          warning: 'domain_unverified',
          created_at: historyAt(-200),
        },
        [
          historyAttempt(2, 'completed', {
            start: -180,
            duration: 152,
            detail: domainsDetail([
              domainResult(
                'brand.test',
                'invalid',
                ['cdn.old-vendor.test'],
                'CNAME 指向 cdn.old-vendor.test，不是集群接入域名'
              ),
              domainResult('www.brand.test', 'verified', ['edge-qa.example.test']),
              domainResult('img.cdn.example.test', 'verified', [
                'edge-qa.example.test',
              ]),
            ]),
            trace: websiteCheckTrace([
              ['brand.test', 'cdn.old-vendor.test'],
              ['www.brand.test', 'edge-qa.example.test'],
              ['img.cdn.example.test', 'edge-qa.example.test'],
            ]),
          }),
          historyAttempt(1, 'failed', {
            start: -199,
            duration: 4000,
            error: '公共 DNS 查询超时',
            retry: true,
            detail: domainsDetail([
              domainResult(
                'brand.test',
                'invalid',
                ['cdn.old-vendor.test'],
                'CNAME 指向 cdn.old-vendor.test，不是集群接入域名'
              ),
              domainResult('www.brand.test', 'unverified', [], '公共 DNS 查询超时'),
            ]),
            trace: websiteCheckTrace([
              ['brand.test', 'cdn.old-vendor.test'],
              ['www.brand.test', '', 'timeout'],
            ]),
          }),
        ]
      ),
    ]
  ),
  seedOperation(
    7013,
    {
      action: 'provider.verify',
      target_type: 'provider',
      target_id: uuid(812),
      target_name: 'zerossl',
      created_at: historyAt(-432_000),
    },
    [
      seedSubtask(
        7231,
        {
          ...work('provider_verify', 'provider', uuid(812), 'zerossl'),
          status: 'completed',
          created_at: historyAt(-432_000),
        },
        [
          historyAttempt(1, 'completed', {
            start: -431_995,
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
      ),
    ]
  ),
  // Failed: QA Edge 08 rejects the new configuration (not retryable); only
  // node reports fail for good.
  seedOperation(
    7014,
    {
      action: 'node.update',
      target_type: 'node',
      target_id: uuid(1005),
      target_name: 'QA Edge 08',
      created_at: historyAt(-3000),
      summary: { sections: ['basic'] },
    },
    [
      seedSubtask(
        7241,
        {
          ...work('node_config', 'node', uuid(1005), 'QA Edge 08'),
          status: 'failed',
          created_at: historyAt(-3000),
        },
        [
          historyAttempt(1, 'failed', {
            start: -2990,
            duration: 2100,
            error: nginxInvalid,
            detail: nodeResult(uuid(1005), 'QA Edge 08', {
              result: 'failed',
              phase: 'validate',
              error_code: 'artifact_invalid',
            }),
            trace: nodeApplyTrace(1, 'validate', nginxInvalid),
          }),
        ]
      ),
    ]
  ),
  // Cancelled without `closed_by`: deleting the certificate provider (a
  // write without a task of its own) cancelled the pending check.
  seedOperation(
    7015,
    {
      action: 'provider.update',
      target_type: 'provider',
      target_kind: 'certificate',
      target_id: removedProviderId,
      target_name: 'letsencrypt',
      created_at: historyAt(-86_400),
      summary: { credentials_changed: true },
    },
    [
      seedSubtask(7251, {
        ...work('provider_verify', 'provider', removedProviderId, 'letsencrypt'),
        status: 'cancelled',
        created_at: historyAt(-86_400),
        finished_at: historyAt(-86_380),
      }),
    ]
  ),
]
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
  // Task center (contract §2.1, §3.1, §4): any invalid parameter is 10001.
  const reject = (message) => fail(message, 400, 10001)
  if (url.pathname === '/api/operations/stream') {
    const value = (name) => query.get(name) || ''
    if (value('page') && !/^[1-9]\d{0,8}$/.test(value('page')))
      reject('page 必须是正整数')
    if (
      value('page_size') &&
      !(/^[1-9]\d{0,2}$/.test(value('page_size')) && Number(value('page_size')) <= 100)
    )
      reject('page_size 必须是 1 到 100 的整数')
    if (
      value('status') &&
      !operationStatuses.includes(value('status')) &&
      !Object.hasOwn(operationGroups, value('status'))
    )
      reject('status 不合法')
    if (value('target_type') && !operationTargetTypes.includes(value('target_type')))
      reject('target_type 不合法')
    if (value('actor') && !['me', 'system'].includes(value('actor')))
      reject('actor 只能是 me 或 system')
    if (value('days') && !['0', '1', '7'].includes(value('days')))
      reject('days 只能是 0、1 或 7')
    if (Buffer.byteLength(value('keyword').trim()) > 200)
      reject('keyword 不能超过 200 字节')
  }
  const operation =
    /^\/api\/operations\/([^/]+)\/(?:stream|attempts\/([^/]+)\/trace\/stream)$/.exec(
      url.pathname
    )
  if (operation) {
    if (!uuidPattern.test(operation[1])) reject('任务 ID 不合法')
    const subtask = query.get('subtask_id')
    if (subtask && !uuidPattern.test(subtask)) reject('subtask_id 不合法')
    if (operation[2] !== undefined && !/^[1-9]\d*$/.test(operation[2]))
      reject('attempt_id 不合法')
  }
}
function findResource(type, id) {
  const lists = {
    website: db.websites,
    cluster: db.clusters,
    node: db.nodes,
    dns_zone: db.zones,
    certificate: db.certificates,
    provider: [...db.dnsProviders, ...db.certificateProviders],
  }
  return lists[type]?.find((item) => item.id === id)
}
/** Name snapshot of a task target or a subtask resource; a certificate
 * provider is named by its code (contract §2.3). */
function resourceName(type, item) {
  if (type === 'website') return item.config.name
  if (type === 'dns_zone') return item.domain
  if (type === 'certificate') return item.domains[0]
  if (type === 'provider') return item.name ?? item.provider
  return item.name
}
/** `target_kind` of a provider target: DNS account or certificate provider. */
const providerKind = (id) =>
  db.dnsProviders.some((provider) => provider.id === id) ? 'dns' : 'certificate'
function findOperation(id) {
  return (
    db.operations.find((operation) => operation.id === id) ??
    fail('任务不存在', 404, 16701)
  )
}
const sortedSubtasks = (operation) =>
  [...operation.subtasks].sort(
    (a, b) => a.created_at.localeCompare(b.created_at) || a.id.localeCompare(b.id)
  )
/** Task status over every subtask, children included (contract §5.2):
 * problems first; attention counts on the success side of a partial
 * failure; merged and cancelled count only when nothing else is left. */
function deriveOperationStatus(statuses) {
  for (const status of ['retrying', 'running', 'queued'])
    if (statuses.includes(status)) return status
  const succeeded = statuses.some(
    (status) => status === 'completed' || status === 'attention_required'
  )
  if (statuses.includes('failed')) return succeeded ? 'partial_failed' : 'failed'
  if (statuses.includes('attention_required')) return 'attention_required'
  if (!statuses.length || statuses.includes('completed')) return 'completed'
  return statuses.every((status) => status === 'merged') ? 'merged' : 'cancelled'
}
/** `operation_dto` (contract §2.3). */
function operationDto(operation) {
  const subtasks = sortedSubtasks(operation),
    statuses = subtasks.map((subtask) => subtask.status),
    settled = subtasks.filter((subtask) =>
      settledStatuses.includes(subtask.status)
    ),
    problem = (subtask) =>
      subtask.status === 'retrying' || subtask.status === 'failed'
  const changes = [
    operation.created_at,
    ...subtasks.flatMap((subtask) => [
      subtask.created_at,
      subtask.running_since,
      subtask.finished_at,
      db.subtaskRecords[subtask.id]?.[0]?.emitted_at ?? '',
    ]),
  ]
    .filter(Boolean)
    .sort()
  return {
    id: operation.id,
    action: operation.action,
    target_type: operation.target_type,
    target_id: operation.target_id,
    target_name: operation.target_name,
    target_deleted: !findResource(operation.target_type, operation.target_id),
    ...(operation.target_type === 'provider'
      ? {
          target_kind:
            operation.target_kind ?? providerKind(operation.target_id),
        }
      : {}),
    actor_type: operation.actor_type,
    actor_name: operation.actor_name,
    created_at: operation.created_at,
    updated_at: changes.at(-1),
    finished_at:
      settled.length === subtasks.length
        ? (settled
            .map((subtask) => subtask.finished_at)
            .sort()
            .at(-1) ?? '')
        : '',
    status: deriveOperationStatus(statuses),
    subtask_count: subtasks.length,
    settled_count: settled.length,
    failed_count: subtasks.filter(problem).length,
    next_attempt_at:
      subtasks
        .filter((subtask) => subtask.status === 'retrying')
        .map((subtask) => subtask.next_attempt_at)
        .filter(Boolean)
        .sort()[0] ?? '',
    error: subtasks.find(problem)?.error ?? '',
    summary: operation.summary,
  }
}
/** `merged_into`: the receiving subtask with its current status. */
function mergedInto({ operation_id, subtask_id }) {
  const owner = db.operations.find((operation) => operation.id === operation_id)
  const receiver = owner?.subtasks.find((subtask) => subtask.id === subtask_id)
  return (
    receiver && {
      operation_id,
      subtask_id,
      action: owner.action,
      target_name: owner.target_name,
      status: receiver.status,
    }
  )
}
/** `subtask_dto` (contract §3.4); optional objects omitted when absent. */
function subtaskDto(subtask) {
  const records = db.subtaskRecords[subtask.id] ?? []
  const failures = records.filter((item) => item.outcome === 'failed').length,
    settled = settledStatuses.includes(subtask.status)
  // Offline only matters while the node still has to apply the work.
  const node =
    ['queued', 'retrying'].includes(subtask.status) &&
    ['node_publish', 'node_config'].includes(subtask.kind)
      ? db.nodes.find((value) => value.id === subtask.resource_id)
      : undefined
  const merged = subtask.merged_into && mergedInto(subtask.merged_into)
  // `closed_by` only without a receiver (contract §3.4).
  const closer =
    !merged &&
    subtask.closed_by &&
    db.operations.find(
      (operation) => operation.id === subtask.closed_by.operation_id
    )
  return {
    id: subtask.id,
    parent_id: subtask.parent_id,
    kind: subtask.kind,
    resource_type: subtask.resource_type,
    resource_id: subtask.resource_id,
    resource_name: subtask.resource_name,
    resource_deleted: !findResource(subtask.resource_type, subtask.resource_id),
    status: subtask.status,
    warning: subtask.status === 'attention_required' ? subtask.warning : '',
    node_offline: Boolean(node) && node.runtime.connection_status !== 'online',
    created_at: subtask.created_at,
    running_since: subtask.status === 'running' ? subtask.running_since : '',
    finished_at: settled ? subtask.finished_at : '',
    // Settled: from creation to settlement.
    duration_ms: settled
      ? Math.max(0, stampMs(subtask.finished_at) - stampMs(subtask.created_at))
      : 0,
    attempt_count: records.length,
    failure_count: failures,
    recovered: subtask.status === 'completed' && failures > 0,
    next_attempt_at: subtask.status === 'retrying' ? subtask.next_attempt_at : '',
    error: ['retrying', 'failed'].includes(subtask.status) ? subtask.error : '',
    ...(merged ? { merged_into: merged } : {}),
    ...(closer
      ? {
          closed_by: {
            operation_id: closer.id,
            action: closer.action,
            target_name: closer.target_name,
          },
        }
      : {}),
  }
}
/** `operation_detail_dto` (contract §3.3). */
function operationDetail(operation) {
  return {
    ...operationDto(operation),
    subtasks: sortedSubtasks(operation).map(subtaskDto),
  }
}
/** `operation_page_dto` (contract §2.2); parameters validated beforehand. */
function operationPage(query) {
  const param = (name) => query.get(name) || ''
  const rows = db.operations.map((operation) => ({
    operation,
    dto: operationDto(operation),
  }))
  const status = param('status'),
    statuses = operationGroups[status] ?? (status ? [status] : undefined),
    actor = param('actor'),
    days = Number(param('days') || 0),
    since = days ? Date.now() - days * 86_400_000 : 0,
    keyword = param('keyword').trim().toLowerCase()
  const list =
    query.get('qa') === 'empty'
      ? []
      : rows
          .filter(
            ({ operation, dto }) =>
              (!statuses || statuses.includes(dto.status)) &&
              (!param('action') || dto.action === param('action')) &&
              (!param('target_type') ||
                dto.target_type === param('target_type')) &&
              (actor !== 'me' ||
                (operation.actor_type === 'user' &&
                  operation.actor_user_id === user.id)) &&
              (actor !== 'system' || operation.actor_type === 'system') &&
              (!since || stampMs(dto.created_at) >= since) &&
              (!keyword ||
                dto.target_name.toLowerCase().includes(keyword) ||
                dto.target_id.toLowerCase().includes(keyword))
          )
          .map(({ dto }) => dto)
          .sort(
            (a, b) =>
              b.created_at.localeCompare(a.created_at) ||
              b.id.localeCompare(a.id)
          )
  const current = Number(param('page') || 1),
    size = Number(param('page_size') || 20),
    count = (group) =>
      rows.filter(({ dto }) => operationGroups[group].includes(dto.status))
        .length
  return {
    list: list.slice((current - 1) * size, current * size),
    total: list.length,
    page: current,
    page_size: size,
    total_pages: Math.ceil(list.length / size),
    active: count('active'),
    attention: count('attention'),
  }
}
/** Background work a write starts (design §1.2), as `[kind, resource type,
 * resource]`; two or three nodes are enough to show a publish per node. */
function plannedSubtasks(action, target) {
  const zoneById = (id) => db.zones.find((zone) => zone.id === id),
    clusterZone = (id) =>
      zoneById(db.clusters.find((cluster) => cluster.id === id)?.dns_zone_id),
    dnsSync = (...zones) =>
      [...new Set(zones.filter(Boolean))].map((zone) => [
        'dns_sync',
        'dns_zone',
        zone,
      ]),
    publish = (clusterId) =>
      db.nodes
        .filter(
          (node) => node.cluster_id === clusterId && node.status === 'enabled'
        )
        .slice(0, 3)
        .map((node) => ['node_publish', 'node', node]),
    websiteZones = () =>
      target.config.domains
        .filter((domain) => domain.dns_mode === 'managed')
        .map((domain) => owningZone(domain.hostname))
  switch (action) {
    case 'website.create':
    case 'website.update':
    case 'website.enable':
    case 'website.disable':
      return [
        ...dnsSync(...websiteZones()),
        ...publish(target.cluster_id),
        ['website_check', 'website', target],
      ]
    case 'website.delete':
      return [...dnsSync(...websiteZones()), ...publish(target.cluster_id)]
    case 'website.check_domains':
      return [['website_check', 'website', target]]
    case 'cluster.update':
      return [...dnsSync(zoneById(target.dns_zone_id)), ...publish(target.id)]
    case 'cluster.delete':
    case 'certificate.delete':
      return dnsSync(zoneById(target.dns_zone_id))
    case 'node.create':
    case 'node.delete':
    case 'node.reset_credentials':
      return dnsSync(clusterZone(target.cluster_id))
    case 'node.update':
      return [
        ['node_config', 'node', target],
        ...dnsSync(clusterZone(target.cluster_id)),
      ]
    case 'system.node_register':
      return [
        ['node_publish', 'node', target],
        ...dnsSync(clusterZone(target.cluster_id)),
      ]
    case 'system.release_upgrade':
      return publish(target.id)
    case 'dns_zone.create':
    case 'dns_zone.update_records':
    case 'dns_zone.sync':
    case 'dns_zone.sync_local':
    case 'dns_zone.sync_remote':
    case 'system.dns_cleanup':
      return dnsSync(target)
    case 'dns_zone.delete':
      return [['dns_delete', 'dns_zone', target]]
    case 'certificate.create':
      return [['certificate_issue', 'certificate', target]]
    case 'certificate.renew':
    case 'system.certificate_auto_renew':
      return [['certificate_renew', 'certificate', target]]
    case 'system.certificate_expired':
      return [
        ...new Set(
          db.websites
            .filter((website) => website.config.certificate_ids.includes(target.id))
            .map((website) => website.cluster_id)
        ),
      ].flatMap(publish)
    case 'provider.update':
    case 'provider.verify':
      return [['provider_verify', 'provider', target]]
  }
  fail(`QA mock 未实现任务动作：${action}`)
}
// Node work merges per node and kind; other work per marker (resource).
const carrierOf = (subtask) =>
  ['node_publish', 'node_config'].includes(subtask.kind)
    ? `${subtask.kind}:${subtask.resource_id}`
    : `${subtask.resource_type}:${subtask.resource_id}`
/** Settles another task's open subtask without running it. */
function settleOpenSubtask(subtask, settledAt, fields) {
  delete subtask.merged_into
  delete subtask.closed_by
  Object.assign(
    subtask,
    { running_since: '', next_attempt_at: '', error: '', finished_at: settledAt },
    fields
  )
}
/** Cancels the open work on a deleted resource. `closedBy` is the deleting
 * task; a write without a task of its own leaves no `closed_by` (D1).
 * Returns whether anything changed. */
function cancelOpenWork(resourceType, resourceId, settledAt, closedBy) {
  let changed = false
  for (const operation of db.operations)
    for (const subtask of operation.subtasks)
      if (
        operationGroups.active.includes(subtask.status) &&
        subtask.resource_type === resourceType &&
        subtask.resource_id === resourceId
      ) {
        settleOpenSubtask(subtask, settledAt, {
          status: 'cancelled',
          ...(closedBy ? { closed_by: { operation_id: closedBy } } : {}),
        })
        changed = true
      }
  return changed
}
/** Records a new task with queued subtasks (D1: none without background
 * work). Open subtasks of other tasks on the same carrier settle as merged
 * into the new ones (design §4); a deletion cancels the open work on the
 * deleted resource. Callers broadcast `operations`. */
function startOperation(action, targetType, target, summary = {}) {
  const planned = plannedSubtasks(action, target)
  if (!planned.length) return undefined
  const now = Date.now(),
    system = action.startsWith('system.')
  const operation = {
    id: randomUUID(),
    action,
    target_type: targetType,
    target_id: target.id,
    target_name: resourceName(targetType, target),
    ...(targetType === 'provider' ? { target_kind: providerKind(target.id) } : {}),
    actor_type: system ? 'system' : 'user',
    actor_user_id: system ? '' : user.id,
    actor_name: system ? '' : user.nickname,
    created_at: historyStamp(now),
    summary,
    // One millisecond apart so the detail keeps the planned order.
    subtasks: planned.map(([kind, resourceType, resource], index) => ({
      id: randomUUID(),
      parent_id: '',
      kind,
      resource_type: resourceType,
      resource_id: resource.id,
      resource_name: resourceName(resourceType, resource),
      status: 'queued',
      warning: '',
      created_at: historyStamp(now + index),
      running_since: '',
      finished_at: '',
      next_attempt_at: '',
      error: '',
    })),
  }
  const receivers = new Map(
    operation.subtasks.map((subtask) => [carrierOf(subtask), subtask])
  )
  for (const other of db.operations)
    for (const subtask of other.subtasks) {
      const receiver = receivers.get(carrierOf(subtask))
      if (receiver && operationGroups.active.includes(subtask.status))
        settleOpenSubtask(subtask, operation.created_at, {
          status: 'merged',
          merged_into: { operation_id: operation.id, subtask_id: receiver.id },
        })
    }
  if (action.endsWith('.delete'))
    cancelOpenWork(targetType, target.id, operation.created_at, operation.id)
  db.operations.push(operation)
  return operation
}
const operationData = (operation) =>
  operation ? { operation_id: operation.id } : undefined
/** Detail, error and trace of a record appended now, by subtask kind;
 * `final` is a node failure the node reports as not retryable. */
function liveRecord(operation, subtask, attempt, failed, final) {
  const { kind } = subtask
  if (kind === 'node_publish' || kind === 'node_config') {
    const phase = final ? 'validate' : 'activate'
    const error = failed
      ? final
        ? nginxInvalid
        : subtask.error || reloadTimeout
      : ''
    return {
      error,
      detail: nodeResult(
        subtask.resource_id,
        subtask.resource_name,
        failed
          ? {
              result: 'failed',
              phase,
              error_code: final ? 'artifact_invalid' : 'apply_failed',
              retryable: !final,
            }
          : {}
      ),
      trace: nodeApplyTrace(attempt, failed ? phase : '', error),
    }
  }
  if (kind === 'website_check') {
    const hosts =
      findResource('website', subtask.resource_id)?.config.domains.map(
        (domain) => domain.hostname
      ) ?? []
    return {
      error: failed ? '公共 DNS 查询超时' : '',
      detail: domainsDetail(
        hosts.map((hostname) =>
          failed
            ? domainResult(hostname, 'unverified', [], '公共 DNS 查询超时')
            : domainResult(hostname, 'verified', ['edge-qa.example.test'])
        )
      ),
      trace: websiteCheckTrace(
        hosts.map((hostname) =>
          failed ? [hostname, '', 'timeout'] : [hostname, 'edge-qa.example.test']
        )
      ),
    }
  }
  if (kind === 'certificate_issue' || kind === 'certificate_renew') {
    const certificate = findResource('certificate', subtask.resource_id),
      domains = certificate?.domains ?? [subtask.resource_name],
      zone =
        db.zones.find((value) => value.id === certificate?.dns_zone_id) ??
        exampleZone,
      children = operation.subtasks.filter(
        (child) => child.parent_id === subtask.id
      ),
      stage = kind === 'certificate_renew' ? 'renew' : 'issue',
      link = (step) =>
        relatedDns(
          operation.id,
          step,
          zone,
          children.find((child) => child.kind === step)
        )
    const links = {
      domains,
      challenge: link('dns_challenge'),
      cleanup: link('dns_challenge_cleanup'),
    }
    return failed
      ? {
          error: `DNS-01 验证超时：_acme-challenge.${domains[0]} 的 TXT 记录在 120 秒内未在权威服务器上生效。`,
          detail: certificateDetail(stage, 'dns_challenge', { domains }),
          trace: certificateRenewFailureTrace({ wait: 116_010, ...links }),
        }
      : {
          error: '',
          detail: certificateDetail(stage, 'completed', { domains }),
          trace: certificateIssueTrace(links),
        }
  }
  if (kind === 'provider_verify')
    return {
      error: failed ? 'Cloudflare API 返回 403：令牌缺少 Zone:Read 权限' : '',
      detail: providerDetail(),
      trace: cloudflareVerificationTrace(failed),
    }
  return {
    error: failed ? 'Cloudflare API 连接失败' : '',
    detail: failed ? dnsDetail({ unchanged_count: 0 }) : dnsDetail(),
    trace: failed ? cloudflareSyncFailureTrace() : cloudflareSyncTrace(),
  }
}
/** Appends a record that ended now and settles the subtask: a failure
 * retries 15 s later (a node may report it final: failed), a success
 * completes it. */
function appendAttempt(operation, subtask, outcome, final = false) {
  const records = (db.subtaskRecords[subtask.id] ??= [])
  const failed = outcome === 'failed',
    duration = 1200,
    attempt = (records[0]?.attempt ?? 0) + 1
  const { error, detail, trace } = liveRecord(
    operation,
    subtask,
    attempt,
    failed,
    final
  )
  const item = historyAttempt(attempt, outcome, {
    start: (Date.now() - historyBase - duration) / 1000,
    duration,
    error,
    retry: failed && !final,
    detail,
    trace,
  })
  records.unshift(item)
  Object.assign(subtask, {
    status: failed ? (final ? 'failed' : 'retrying') : 'completed',
    warning: '',
    running_since: '',
    finished_at: failed && !final ? '' : item.emitted_at,
    next_attempt_at: item.next_attempt_at,
    error,
  })
  return item
}
/** Contract §3.2 order: the changed snapshots first, then `attempt` (plain
 * JSON) to the detail streams expanding that subtask. */
function publishAttempt(operation, subtask, item) {
  broadcast(['operations'])
  for (const stream of activeStreams.values())
    if (
      stream.url.pathname === `/api/operations/${operation.id}/stream` &&
      stream.url.searchParams.get('subtask_id') === subtask.id &&
      // Only after the first `history` (contract §3.2).
      stream.framesByEvent?.has('history')
    )
      emit(
        stream,
        'attempt',
        { subtask_id: subtask.id, item, summary: historySummary(subtask) },
        { force: true }
      )
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
    },
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
  if (path === '/api/operations/stream') return operationPage(query)
  if (part[1] === 'operations') {
    const operation = findOperation(part[2])
    if (part[3] === 'attempts') {
      if (operationTraceMode === 'error')
        fail('QA 场景：执行详情读取失败', 500, 10004)
      // An attempt of another task reads as not found.
      for (const subtask of operation.subtasks) {
        const item = db.subtaskRecords[subtask.id]?.find(
          (record) => record.trace.attempt_id === part[4]
        )
        if (item) return traceSnapshot(subtask, item)
      }
      fail('执行记录不存在', 404, 16706)
    }
    const subtaskId = query.get('subtask_id')
    if (subtaskId && !operation.subtasks.some((item) => item.id === subtaskId))
      fail('子任务不存在', 404, 16705)
    return operationDetail(operation)
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
    // A detail stream expanding a subtask sends its `history` after the
    // first and every recovery (forced) snapshot only; new records follow
    // as `attempt` events.
    const subtaskId = stream.url.searchParams.get('subtask_id')
    if (
      subtaskId &&
      /^\/api\/operations\/[^/]+\/stream$/.test(stream.url.pathname) &&
      (force || !stream.framesByEvent.has('history'))
    )
      emit(
        stream,
        'history',
        envelope(
          subtaskHistory(
            findOperation(stream.url.pathname.split('/')[3]).subtasks.find(
              (subtask) => subtask.id === subtaskId
            )
          )
        ),
        { force }
      )
  } catch (error) {
    const before = stream.eventCount
    emit(
      stream,
      'resource-error',
      errorEnvelope(error.message, error.code ?? 40001),
      { force }
    )
    // 16705: both `snapshot` and `history` are answered with the error; the
    // connection stays open (contract §3.1, §7).
    if (error.code === 16705 && stream.eventCount > before)
      emit(
        stream,
        'resource-error',
        errorEnvelope(error.message, error.code),
        { force: true }
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
      operations: db.operations.length,
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
    operationTraceMode = 'reset'
    for (const stream of targets)
      if (!/\/(?:logs|access-logs)\/stream$/.test(stream.url.pathname))
        emitSnapshot(stream, true)
    return { reset: true }
  }
  // Subtask named by `operation`/`subtask`, else the default, else the first
  // open subtask of `operation`.
  const pickSubtask = ([defaultOperation, defaultSubtask]) => {
    const operationId = query.get('operation')
    const operation = findOperation(operationId ?? defaultOperation)
    const subtaskId =
      query.get('subtask') ?? (operationId ? undefined : defaultSubtask)
    const subtask = subtaskId
      ? (operation.subtasks.find((item) => item.id === subtaskId) ??
        fail('子任务不存在', 404, 16705))
      : (sortedSubtasks(operation).find((item) =>
          operationGroups.active.includes(item.status)
        ) ?? fail('任务没有进行中的子任务', 409))
    if (!operationGroups.active.includes(subtask.status))
      fail('子任务已结算', 409)
    return { operation, subtask }
  }
  const nodeWork = (subtask) =>
    subtask.kind === 'node_publish' || subtask.kind === 'node_config'
  if (event === 'operation-claim') {
    // A worker claims queued or retrying sync-type work (default: the
    // backup.test sync); every claim restarts `running_since`. Node work is
    // never running (contract §3.4).
    const { operation, subtask } = pickSubtask([uuid(7010), uuid(7201)])
    if (nodeWork(subtask)) fail('节点类子任务没有执行中状态', 409)
    Object.assign(subtask, {
      status: 'running',
      running_since: historyStamp(Date.now()),
      next_attempt_at: '',
    })
    broadcast(['operations'])
    return {
      operation_id: operation.id,
      subtask_id: subtask.id,
      running_since: subtask.running_since,
      operation_status: operationDto(operation).status,
    }
  }
  if (event === 'operation-attempt' || event === 'operation-complete') {
    // Defaults: a failed record on the retrying QA Edge 02 publish, or the
    // running domain check of 7007 completes. `retryable=0` makes a node
    // failure final (failed).
    const complete = event === 'operation-complete'
    const { operation, subtask } = pickSubtask(
      complete ? [uuid(7007), uuid(7175)] : [uuid(7001), uuid(7113)]
    )
    const outcome = complete ? 'completed' : (query.get('outcome') ?? 'failed')
    if (outcome !== 'failed' && outcome !== 'completed')
      fail('outcome 只能是 failed 或 completed')
    const final = query.get('retryable') === '0'
    if (final && !nodeWork(subtask))
      fail('只有节点类子任务会不可重试地失败', 409)
    const item = appendAttempt(operation, subtask, outcome, final)
    publishAttempt(operation, subtask, item)
    return {
      operation_id: operation.id,
      subtask_id: subtask.id,
      attempt: item.attempt,
      attempt_id: item.trace.attempt_id,
      subtask_status: subtask.status,
      operation_status: operationDto(operation).status,
    }
  }
  if (event === 'operation-create') {
    const action = query.get('action') ?? 'website.update'
    const systemTargets = {
      'system.certificate_auto_renew': 'certificate',
      'system.certificate_expired': 'certificate',
      'system.node_register': 'node',
      'system.release_upgrade': 'cluster',
      'system.dns_cleanup': 'dns_zone',
    }
    const defaults = {
      website: brandWebsiteId,
      cluster: clusterId,
      node: nodeId,
      dns_zone: zoneId,
      certificate: certificateId,
      provider: dnsProviderId,
    }
    const targetType = systemTargets[action] ?? action.split('.')[0]
    if (!operationTargetTypes.includes(targetType))
      fail(`QA mock 未实现任务动作：${action}`)
    const target =
      findResource(targetType, query.get('target') ?? defaults[targetType]) ??
      fail('资源不存在', 404)
    const operation =
      startOperation(action, targetType, target) ??
      fail('该操作没有后台工作，不产生任务', 409)
    broadcast(['operations'])
    return {
      operation_id: operation.id,
      action,
      subtask_ids: operation.subtasks.map((subtask) => subtask.id),
    }
  }
  if (event === 'operation-history-reset') {
    // Retention cleanup: drops all but the newest `keep` records of one
    // subtask (default: backup.test sync) and re-sends its `history`.
    const operation = findOperation(query.get('operation') ?? uuid(7010))
    const subtaskId = query.get('subtask') ?? sortedSubtasks(operation)[0].id
    const subtask =
      operation.subtasks.find((item) => item.id === subtaskId) ??
      fail('子任务不存在', 404, 16705)
    const keep = Math.max(0, Number(query.get('keep')) || 0)
    db.subtaskRecords[subtask.id] = (
      db.subtaskRecords[subtask.id] ?? []
    ).slice(0, keep)
    for (const stream of activeStreams.values())
      if (
        stream.url.pathname === `/api/operations/${operation.id}/stream` &&
        stream.url.searchParams.get('subtask_id') === subtask.id
      )
        emit(stream, 'history', envelope(subtaskHistory(subtask)), {
          force: true,
        })
    broadcast(['operations'])
    return {
      operation_id: operation.id,
      subtask_id: subtask.id,
      kept: db.subtaskRecords[subtask.id].length,
    }
  }
  if (event === 'operation-trace') {
    // Traces are one-shot snapshots: `error` fails the next trace reads
    // (expand an attempt or 重试), `reset` restores them.
    const mode = query.get('mode') ?? 'reset'
    operationTraceMode = mode === 'error' ? 'error' : 'reset'
    return { mode: operationTraceMode }
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
    // Node goes online/offline: list, detail, cluster, overview and task
    // snapshots (`node_offline`) change.
    const node = find(db.nodes, query.get('id') ?? nodeId)
    const status = query.get('status') === 'online' ? 'online' : 'offline'
    node.runtime.connection_status = status
    if (status === 'online') node.runtime.last_heartbeat_at = nextTime()
    refreshRelations()
    broadcast(['nodes', 'clusters', 'operations'])
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
      operations: db.operations,
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
    // A node's connection status shows as `node_offline` in tasks.
    broadcast([resource, ...(resource === 'nodes' ? ['operations'] : [])])
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
// Task targets of the write routes, and the actions without a request body
// to inspect (contract §5.1, D1).
const resourceTargetTypes = {
  websites: 'website',
  clusters: 'cluster',
  nodes: 'node',
  'dns-zones': 'dns_zone',
  certificates: 'certificate',
  'providers/dns': 'provider',
  'providers/certificate': 'provider',
}
const creationActions = {
  websites: 'website.create',
  nodes: 'node.create',
  'dns-zones': 'dns_zone.create',
  certificates: 'certificate.create',
}
const deletionActions = {
  websites: 'website.delete',
  clusters: 'cluster.delete',
  nodes: 'node.delete',
  'dns-zones': 'dns_zone.delete',
  certificates: 'certificate.delete',
}
const sectionOrder = [
  'basic',
  'domains',
  'origins',
  'health_check',
  'access_log',
  'https',
  'compression',
  'rules',
  'cluster',
  'dns',
  'endpoints',
  'status',
]
/** `summary.sections` (design §2.5): sections whose keys changed. */
function changedSections(before, after, sectionOf) {
  const sections = new Set()
  for (const key of new Set([...Object.keys(before), ...Object.keys(after)]))
    if (JSON.stringify(before[key]) !== JSON.stringify(after[key]))
      sections.add(sectionOf(key))
  return sectionOrder.filter((section) => sections.has(section))
}
const websiteSection = (key) =>
  key === 'name'
    ? 'basic'
    : ['domains', 'cluster', 'status'].includes(key)
      ? key
      : key === 'route_rules'
        ? 'rules'
        : key.startsWith('health_check') || key.endsWith('healthy_threshold')
          ? 'health_check'
          : key.startsWith('access_log')
            ? 'access_log'
            : key.startsWith('response_compression')
              ? 'compression'
              : [
                    'https_enabled',
                    'certificate_ids',
                    'minimum_tls_version',
                    'force_https',
                  ].includes(key) || key.startsWith('hsts')
                ? 'https'
                : 'origins'
const nodeSection = (key) =>
  ({ cluster_id: 'cluster', config: 'endpoints', status: 'status' })[key] ??
  'basic'
const clusterSection = (key) =>
  ({ dns_zone_id: 'dns', hostname_prefix: 'dns', status: 'status' })[key] ??
  'basic'
const submitted = (item, input) =>
  Object.fromEntries(Object.keys(input).map((key) => [key, item[key]]))
/** `summary.record_counts` of a record set save, matched by record id. */
function recordCounts(before, after) {
  const previous = new Map(
    before.map((record) => [record.id, JSON.stringify(record)])
  )
  const kept = new Set(after.map((record) => record.id))
  return {
    created: after.filter((record) => !previous.has(record.id)).length,
    updated: after.filter(
      (record) =>
        previous.has(record.id) &&
        previous.get(record.id) !== JSON.stringify(record)
    ).length,
    deleted: before.filter((record) => !kept.has(record.id)).length,
  }
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
    let started
    if (part[4] !== 'reveal') {
      requireRevision(req, node)
      touch(node)
      credentials.set(id, issueCredentials())
      started = startOperation('node.reset_credentials', 'node', node)
      broadcast(['nodes', ...(started ? ['operations'] : [])])
    }
    return json(
      res,
      {
        ...(credentials.get(id) ??
          credentials.set(id, issueCredentials()).get(id)),
        revision: node.revision,
        ...operationData(started),
      },
      '凭据已获取',
      node.revision
    )
  }
  if (req.method === 'POST' && !id) {
    const item = createResource(resource, input, url)
    list.push(item)
    refreshRelations()
    const started =
      creationActions[resource] &&
      startOperation(
        creationActions[resource],
        resourceTargetTypes[resource],
        item
      )
    broadcast([resource, 'clusters', ...(started ? ['operations'] : [])])
    if (resource === 'nodes') {
      const issued = issueCredentials()
      credentials.set(item.id, issued)
      return json(
        res,
        {
          ...issued,
          id: item.id,
          revision: item.revision,
          ...operationData(started),
        },
        '节点已创建',
        item.revision
      )
    }
    // Contract: creation responses carry the new resource id.
    if (resource === 'websites')
      return json(
        res,
        { id: item.id, revision: item.revision, ...operationData(started) },
        '资源已创建',
        item.revision
      )
    return json(res, operationData(started), '资源已创建', item.revision)
  }
  const item = find(list, id)
  if (req.method === 'POST' && action === 'dns-probe') {
    item.runtime.domain_states = item.config.domains.map((domain) => ({
      id: domain.id,
      access_protocol: item.config.https_enabled ? 'https' : 'http',
      resolution_status: 'resolved',
      last_verified_at: nextTime(),
    }))
    const started = startOperation('website.check_domains', 'website', item)
    broadcast(['websites', ...(started ? ['operations'] : [])])
    return json(res, operationData(started), 'DNS 检查已提交')
  }
  if (!(req.method === 'POST' && action === 'sync')) requireRevision(req, item)
  if (req.method === 'DELETE') {
    rejectDelete(resource, id)
    list.splice(list.indexOf(item), 1)
    refreshRelations()
    const started =
      deletionActions[resource] &&
      startOperation(
        deletionActions[resource],
        resourceTargetTypes[resource],
        item
      )
    // Provider deletion has no task (D1) but cancels the pending checks,
    // which then carry no `closed_by`.
    const cancelled =
      resource.startsWith('providers/') &&
      cancelOpenWork('provider', item.id, historyStamp(Date.now()))
    broadcast([
      resource,
      'clusters',
      ...(started || cancelled ? ['operations'] : []),
    ])
    return json(res, operationData(started), '资源已删除', item.revision + 1)
  }
  // `[action, summary]` of the task this write starts, if any.
  let task
  if (req.method === 'PUT') {
    if (resource === 'websites') {
      const targetCluster =
        url.searchParams.get('cluster_id') ?? item.cluster_id
      find(db.clusters, targetCluster)
      validateWebsiteDomains(input.config)
      const sections = changedSections(
        { ...item.config, cluster: item.cluster_id, status: item.status },
        { ...input.config, cluster: targetCluster, status: input.status },
        websiteSection
      )
      task = [
        sections.length === 1 && sections[0] === 'status'
          ? input.status === 'enabled'
            ? 'website.enable'
            : 'website.disable'
          : 'website.update',
        sections.length ? { sections } : {},
      ]
      item.cluster_id = targetCluster
      item.status = input.status
      item.config = input.config
    } else if (resource === 'dns-zones') {
      task = [
        'dns_zone.update_records',
        {
          record_counts: recordCounts(
            item.config.records ?? [],
            input.records ?? []
          ),
        },
      ]
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
        task = ['provider.update', { credentials_changed: true }]
      }
    } else if (resource === 'providers/certificate') {
      if (
        input.access_key ||
        input.credential_mode !== item.credential_mode ||
        (input.account_email && input.account_email !== item.account_email)
      )
        task = ['provider.update', { credentials_changed: true }]
      item.credential_mode = input.credential_mode
      if (input.account_email) item.account_email = input.account_email
      if (input.access_key)
        item.access_key_hint = `****${input.access_key.slice(-4)}`
      item.status = 'unverified'
    } else {
      const sections = changedSections(
        submitted(item, input),
        input,
        resource === 'nodes' ? nodeSection : clusterSection
      )
      task = [
        resource === 'nodes' ? 'node.update' : 'cluster.update',
        sections.length ? { sections } : {},
      ]
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
    task = ['provider.verify', {}]
  } else if (req.method === 'POST' && action === 'renew') {
    item.status = 'renewing'
    item.sync_status = 'queued'
    task = ['certificate.renew', {}]
  } else if (req.method === 'POST' && action === 'sync') {
    const policy = input.conflict_policy
    task = ['local', 'remote'].includes(policy)
      ? [`dns_zone.sync_${policy}`, { conflict_policy: policy }]
      : ['dns_zone.sync', {}]
    item.sync_status = 'synced'
    item.synced_revision = item.desired_revision
    item.last_synced_at = nextTime()
    item.runtime.conflicts = []
    delete item.last_error
  } else fail('QA 操作未实现', 404)
  if (req.method === 'PUT') touch(item)
  else item.updated_at = nextTime()
  refreshRelations()
  // Certificate renewal settings (PUT) start no background work (D1).
  const started =
    task &&
    startOperation(task[0], resourceTargetTypes[resource], item, task[1])
  broadcast([
    resource,
    'clusters',
    'websites',
    ...(started ? ['operations'] : []),
  ])
  return json(res, operationData(started), '资源已更新', item.revision)
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
