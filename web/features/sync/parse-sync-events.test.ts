import { describe, expect, it } from 'vitest'
import { parseSyncEvents } from './parse-sync-events'

const event = {
  sequence: 1,
  resource_type: 'dns_zone',
  resource_id: 'zone-1',
  operation: 'sync',
  version: 2,
  outcome: 'completed',
  emitted_at: '2026-09-08T00:00:00Z',
}
const encode = (list: unknown[]) =>
  JSON.stringify({
    code: 0,
    message: '',
    data: { list, cursor: 1, has_more: false },
  })

describe('sync event boundary', () => {
  it('accepts complete event batches and empty pages', () => {
    expect(parseSyncEvents(encode([event]))).toEqual([event])
    expect(parseSyncEvents(encode([]))).toEqual([])
  })
  it('rejects malformed envelopes and page metadata', () => {
    for (const raw of [
      '{',
      'null',
      '{}',
      JSON.stringify({ code: 1, message: 'failed' }),
      JSON.stringify({ code: 0, message: '', data: { list: [event] } }),
    ]) {
      expect(() => parseSyncEvents(raw)).toThrow()
    }
  })
  it('rejects an entire batch before invalid resource keys reach cache refresh', () => {
    for (const invalid of [
      null,
      { ...event, resource_type: 'unknown' },
      { ...event, outcome: 'unknown' },
      { ...event, sequence: '1' },
      { ...event, version: 1.5 },
    ]) {
      expect(() => parseSyncEvents(encode([event, invalid]))).toThrow()
    }
    expect(parseSyncEvents(encode([event]))).toEqual([event])
  })
})
