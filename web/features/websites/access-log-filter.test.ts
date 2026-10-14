import { describe, expect, it } from 'vitest'
import { accessLogStatusTone, filterAccessLogs } from './access-log-filter'
import type { AccessLog } from './access-log-schema'

function log(overrides: Partial<AccessLog>): AccessLog {
  return {
    id: 'log',
    occurred_at: '2026-10-14T00:00:00Z',
    node_id: 'node',
    node_name: 'QA Edge 01',
    client_ip: '203.0.113.10',
    protocol: 'HTTP/1.1',
    method: 'GET',
    host: 'qa.example.test',
    target: '/index.html',
    status_code: 200,
    request_bytes: 100,
    response_bytes: 200,
    duration_ms: 5,
    request_body_truncated: false,
    ...overrides,
  }
}

describe('filterAccessLogs', () => {
  const logs = [
    log({ id: 'a' }),
    log({ id: 'b', method: 'POST', status_code: 502, target: '/api/orders' }),
    log({ id: 'c', status_code: 404, client_ip: '198.51.100.7' }),
  ]

  it('applies method, status class and keyword together', () => {
    expect(filterAccessLogs(logs, { method: 'POST' }).map((x) => x.id)).toEqual(
      ['b']
    )
    expect(
      filterAccessLogs(logs, { status_class: '4xx' }).map((x) => x.id)
    ).toEqual(['c'])
    expect(
      filterAccessLogs(logs, { keyword: ' API/ORDERS ' }).map((x) => x.id)
    ).toEqual(['b'])
    expect(
      filterAccessLogs(logs, { keyword: '198.51' }).map((x) => x.id)
    ).toEqual(['c'])
    expect(
      filterAccessLogs(logs, { method: 'GET', status_class: '5xx' })
    ).toEqual([])
  })

  it('returns every log without filters', () => {
    expect(filterAccessLogs(logs, {})).toHaveLength(3)
  })
})

describe('accessLogStatusTone', () => {
  it('maps response classes to tones', () => {
    expect(accessLogStatusTone(204)).toBe('success')
    expect(accessLogStatusTone(301)).toBe('accent')
    expect(accessLogStatusTone(429)).toBe('warning')
    expect(accessLogStatusTone(503)).toBe('danger')
  })
})
