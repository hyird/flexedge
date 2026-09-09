import { describe, expect, it } from 'vitest'
import { parseWebsiteDashboard } from './dashboard-schema'

const dashboard = {
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
    },
  ],
  daily: [],
  status_codes: [{ label: '200', request_count: 1, response_bytes: 20 }],
  methods: [],
  countries: [],
  hosts: [],
  referers: [],
  paths: [],
  client_ips_by_bytes: [],
  client_ips_by_requests: [],
}
const encode = (data: unknown) => JSON.stringify({ code: 0, message: '', data })

describe('website dashboard contract', () => {
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
    const parsed = parseWebsiteDashboard(
      encode({
        ...dashboard,
        client_ips_by_bytes: [ip, unknown],
        client_ips_by_requests: [ip],
      })
    )
    expect(parsed.client_ips_by_bytes).toEqual([ip, unknown])
    expect(parsed.client_ips_by_requests[0].as_name).toBe(ip.as_name)
    expect(() =>
      parseWebsiteDashboard(
        encode({ ...dashboard, client_ips_by_bytes: [{ ...ip, as_name: 123 }] })
      )
    ).toThrow()
  })
  it('accepts a complete snapshot including empty rankings', () => {
    expect(parseWebsiteDashboard(encode(dashboard))).toEqual(dashboard)
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
      expect(() => parseWebsiteDashboard(encode(data))).toThrow()
    }
  })
  it('rejects transport envelopes that do not contain a successful snapshot', () => {
    for (const raw of [
      '{',
      '{}',
      JSON.stringify({ code: 1, message: 'failed', data: dashboard }),
    ]) {
      expect(() => parseWebsiteDashboard(raw)).toThrow()
    }
  })
})
