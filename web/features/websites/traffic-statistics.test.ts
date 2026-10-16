import { describe, expect, it } from 'vitest'
import { todayTrafficStatistics } from './traffic-statistics'

const summary = {
  previous_month_peak_bps: 0,
  current_month_peak_bps: 0,
  today_peak_bps: 0,
  current_bandwidth_bps: 0,
  today_unique_ips: 4,
  today_response_bytes: 1024,
}
const bucket = (request_count: number) => ({
  timestamp: '2026-10-15T16:00:00Z',
  request_count,
  response_bytes: 1024,
  bandwidth_bps: 0,
})

describe('today traffic statistics', () => {
  it('uses the current console-day bucket rather than previous days', () => {
    expect(
      todayTrafficStatistics({ summary, daily: [bucket(1000), bucket(8)] })
    ).toEqual({ requests: 8, requestsPerIp: 2 })
  })

  it('retains fractional requests per IP', () => {
    expect(
      todayTrafficStatistics({
        summary: { ...summary, today_response_bytes: 11 },
        daily: [bucket(6)],
      })
    ).toEqual({ requests: 6, requestsPerIp: 1.5 })
  })

  it('does not invent totals or averages when the current bucket is absent', () => {
    expect(todayTrafficStatistics({ summary, daily: [] })).toEqual({
      requests: undefined,
      requestsPerIp: undefined,
    })
  })

  it('keeps zero requests and avoids division by a zero IP count', () => {
    expect(
      todayTrafficStatistics({
        summary: { ...summary, today_response_bytes: 0, today_unique_ips: 0 },
        daily: [bucket(0)],
      })
    ).toEqual({
      requests: 0,
      requestsPerIp: undefined,
    })
  })

  it('does not divide by unavailable IP counts while retaining request totals', () => {
    expect(
      todayTrafficStatistics({
        summary: { ...summary, today_unique_ips: 0 },
        daily: [bucket(8)],
      })
    ).toEqual({
      requests: 8,
      requestsPerIp: undefined,
    })
  })
})
