import { describe, expect, it } from 'vitest'
import { parseNodeLogs } from '@/features/nodes/node-log-schema'
import {
  accessLogPageSchema,
  parseAccessLogs,
} from '@/features/websites/access-log-schema'

const encode = (list: unknown[]) =>
  JSON.stringify({ code: 0, message: '', data: { list } })
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

  it('rejects malformed JSON, envelopes and business failures', () => {
    for (const payload of [
      '{',
      'null',
      '[]',
      JSON.stringify({ code: 1, message: 'failure', data: { list: [] } }),
      JSON.stringify({ code: 0, message: '', data: {} }),
    ]) {
      expect(() => parseNodeLogs(payload)).toThrow()
    }
  })

  it('accepts omitted optional access log fields without fabricating values', () => {
    expect(parseAccessLogs(encode([accessLog]))).toEqual([accessLog])
    const detailed = {
      ...accessLog,
      request_headers: 'Accept: */*',
      request_body: '',
      client_ip_location: '测试',
    }
    expect(parseAccessLogs(encode([detailed]))).toEqual([detailed])
    expect(() =>
      parseAccessLogs(encode([{ ...accessLog, protocol: null }]))
    ).toThrow()
    expect(() =>
      parseAccessLogs(encode([{ ...accessLog, status_code: '200' }]))
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
