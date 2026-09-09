import { expect, test } from 'vitest'
import { recordsSchema } from './records-form'

const record = {
  id: '29b9df01-e8bb-4f33-87e5-dfef34c77f8f',
  type: 'TXT',
  name: '@',
  content: '  exact text  ',
  ttl: 600,
  proxied: false,
  line_code: 'default',
}
test('record editing preserves TXT content byte for byte', () => {
  const parsed = recordsSchema.parse({ records: [record] })
  expect(parsed.records[0].content).toBe(record.content)
  expect(
    recordsSchema.parse({ records: [{ ...record, content: ' ' }] }).records[0]
      .content
  ).toBe(' ')
})
test('record editing rejects empty content and out of range values', () => {
  for (const patch of [
    { content: '' },
    { content: 'x'.repeat(4097) },
    { ttl: 0 },
    { ttl: 86401 },
    { priority: -1 },
    { priority: 65536 },
  ]) {
    expect(
      recordsSchema.safeParse({ records: [{ ...record, ...patch }] }).success
    ).toBe(false)
  }
  expect(
    recordsSchema.safeParse({
      records: [
        { ...record, content: 'x'.repeat(4096), ttl: 86400, priority: 65535 },
      ],
    }).success
  ).toBe(true)
})
