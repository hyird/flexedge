import { describe, expect, it } from 'vitest'
import { parseWebsiteDashboard } from './dashboard-schema'

const dashboard = {
  statistics: {
    request_count: 1,
    response_bytes: 20,
    request_bytes: 10,
    current_qps: 0,
    peak_qps: 0,
    average_duration_ms: 20,
    p95_duration_ms: 20,
    status_4xx_count: 0,
    status_5xx_count: 0,
    cache_known_requests: 1,
    cache_known_bytes: 20,
    cache_hit_requests: 1,
    cache_hit_bytes: 20,
    cache_memory_requests: 1,
    cache_disk_requests: 0,
    cache_revalidated_requests: 0,
    cache_miss_requests: 0,
    cache_bypass_requests: 0,
  },
  cache_states: [],
  cache_reasons: [],
  protocols: [],
  summary: {
    previous_month_peak_bps: 0,
    current_month_peak_bps: 1,
    today_peak_bps: 1,
    current_bandwidth_bps: 0,
    today_unique_ips: 1,
    today_response_bytes: 20,
  },
  hourly: [
    {
      timestamp: '2026-09-08T00:00:00Z',
      request_count: 1,
      response_bytes: 20,
      bandwidth_bps: 0,
      cache_hit_requests: 1,
      cache_hit_bytes: 20,
      duration_ms: 20,
      error_requests: 0,
    },
  ],
  daily: [],
  status_codes: [{ label: '200', request_count: 1, response_bytes: 20 }],
  methods: [],
  countries: [],
  asns: [],
  hosts: [],
  referers: [],
  paths: [],
  client_ips_by_bytes: [],
  client_ips_by_requests: [],
}

describe('website dashboard contract', () => {
  it('preserves ASN totals independently of the capped client IP rankings', () => {
    const asns = [
      { label: 'AS4134 Chinanet', request_count: 123, response_bytes: 4096 },
      { label: '未知ASN', request_count: 7, response_bytes: 512 },
    ]
    expect(parseWebsiteDashboard({ ...dashboard, asns }).asns).toEqual(asns)
    expect(() =>
      parseWebsiteDashboard({
        ...dashboard,
        asns: [{ ...asns[0], request_count: '123' }],
      })
    ).toThrow()
  })
  it('preserves full AS names for both IP rankings and permits unavailable metadata', () => {
    const ip = {
      label: '203.0.113.1 · 中国',
      request_count: 3,
      response_bytes: 120,
      asn: 'AS141425',
      as_name: 'China Mobile Group Guangdong communications corporation',
    }
    const unknown = {
      ...ip,
      label: '192.0.2.1 · 未知地区',
      asn: '',
      as_name: '',
    }
    const parsed = parseWebsiteDashboard({
      ...dashboard,
      client_ips_by_bytes: [ip, unknown],
      client_ips_by_requests: [ip],
    })
    expect(parsed.client_ips_by_bytes).toEqual([ip, unknown])
    expect(parsed.client_ips_by_requests[0].as_name).toBe(ip.as_name)
    expect(() =>
      parseWebsiteDashboard({
        ...dashboard,
        client_ips_by_bytes: [{ ...ip, as_name: 123 }],
      })
    ).toThrow()
  })
  it('accepts a complete snapshot including empty rankings', () => {
    expect(parseWebsiteDashboard(dashboard)).toEqual(dashboard)
  })
  it('rejects malformed nested values before chart rendering', () => {
    for (const data of [
      null,
      {},
      { ...dashboard, summary: null },
      { ...dashboard, hourly: [null] },
      {
        ...dashboard,
        methods: [{ label: 200, request_count: 1, response_bytes: 0 }],
      },
      { ...dashboard, daily: [{ ...dashboard.hourly[0], bandwidth_bps: '0' }] },
    ]) {
      expect(() => parseWebsiteDashboard(data)).toThrow()
    }
  })
})
