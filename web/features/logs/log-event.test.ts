import { describe, expect, it } from 'vitest'
import { parseNodeLogs } from '@/features/nodes/node-log-schema'
import {
  accessLogPageSchema,
  parseAccessLogs,
} from '@/features/websites/access-log-schema'
import { liveLogCapacity, mergeLiveLogs } from './log-event'

const encode = (list: unknown[]) => ({ list })
const nodeLog = {
  id: 'one',
  occurred_at: '2026-09-08T00:00:00Z',
  level: 'info',
  category: 'runtime',
  message: 'running',
}
const accessLog = {
  id: 'one',
  occurred_at: '2026-09-08T00:00:00Z',
  node_id: 'node',
  node_name: 'Node',
  protocol: 'h2',
  method: 'GET',
  host: 'example.test',
  target: '/',
  status_code: 200,
  request_bytes: 0,
  response_bytes: 100,
  duration_ms: 2,
  request_body_truncated: false,
}

describe('log event contracts', () => {
  it('accepts node records and rejects a batch with an invalid required field', () => {
    expect(parseNodeLogs(encode([nodeLog]))).toEqual([nodeLog])
    expect(() =>
      parseNodeLogs(encode([nodeLog, { ...nodeLog, level: null }]))
    ).toThrow()
    expect(() =>
      parseNodeLogs(encode([{ id: 'one', occurred_at: 'now' }]))
    ).toThrow()
  })

  it('rejects frames without a record list', () => {
    for (const data of [null, [], {}, { list: null }, { list: {} }]) {
      expect(() => parseNodeLogs(data)).toThrow()
    }
  })

  it('merges batches by id, newest first, within the capacity', () => {
    const at = (id: string, minute: number) => ({
      id,
      occurred_at: `2026-09-08T00:${String(minute).padStart(2, '0')}:00Z`,
    })
    expect(
      mergeLiveLogs([at('a', 1), at('b', 2)], [at('b', 2), at('c', 3)])
    ).toEqual([at('c', 3), at('b', 2), at('a', 1)])
    const many = Array.from({ length: liveLogCapacity + 5 }, (_, index) => ({
      id: String(index),
      occurred_at: new Date(Date.UTC(2026, 0, 1, 0, 0, index)).toISOString(),
    }))
    const merged = mergeLiveLogs(undefined, many)
    expect(merged).toHaveLength(liveLogCapacity)
    expect(merged[0].id).toBe(String(liveLogCapacity + 4))
  })

  it('accepts omitted optional access log fields without fabricating values', () => {
    expect(parseAccessLogs(encode([accessLog]))).toEqual([accessLog])
    const detailed = {
      ...accessLog,
      request_headers: 'Accept: */*',
      request_body: '',
      client_ip_location: '测试',
      client_ip_asn: 'AS4134',
      client_ip_as_name: 'Chinanet',
    }
    expect(parseAccessLogs(encode([detailed]))).toEqual([detailed])
    expect(() =>
      parseAccessLogs(encode([{ ...accessLog, protocol: null }]))
    ).toThrow()
    expect(() =>
      parseAccessLogs(encode([{ ...accessLog, status_code: '200' }]))
    ).toThrow()
    expect(() =>
      parseAccessLogs(encode([{ ...accessLog, client_ip_asn: 4134 }]))
    ).toThrow()
  })

  it('uses the same access record contract for history pages', () => {
    const page = {
      list: [accessLog],
      total: 1,
      page: 1,
      page_size: 50,
      total_pages: 1,
    }
    expect(accessLogPageSchema.parse(page)).toEqual(page)
    expect(() =>
      accessLogPageSchema.parse({
        ...page,
        list: [{ ...accessLog, request_body_truncated: undefined }],
      })
    ).toThrow()
  })
})
