import { expect, test } from 'vitest'
import {
  applyRecordChange,
  recordFormSchema,
  recordFormValues,
  type RecordFormValues,
} from './records-form'
import type { DnsRecord } from './types'

const values: RecordFormValues = {
  type: 'TXT',
  name: '',
  content: '  exact text  ',
  ttl: '600',
  priority: '10',
  proxied: false,
  line_code: 'default',
}
const issues = (provider: string, patch: Partial<RecordFormValues>) => {
  const result = recordFormSchema(provider).safeParse({ ...values, ...patch })
  return result.success
    ? {}
    : Object.fromEntries(
        result.error.issues.map((issue) => [issue.path[0], issue.message])
      )
}

test('record editing preserves TXT content byte for byte and maps an empty name to the apex', () => {
  const parsed = recordFormSchema('aliyun').parse(values)
  expect(parsed).toEqual({
    type: 'TXT',
    name: '@',
    content: '  exact text  ',
    ttl: 600,
    proxied: false,
    line_code: 'default',
  })
  expect(
    recordFormSchema('aliyun').parse({ ...values, content: ' ' }).content
  ).toBe(' ')
})

test('record editing reports every invalid field with its own message', () => {
  expect(issues('cloudflare', { content: '' })).toEqual({
    content: '请输入记录值',
  })
  expect(issues('cloudflare', { content: 'x'.repeat(4097) })).toEqual({
    content: '记录值最多 4096 个字符',
  })
  expect(
    issues('cloudflare', { ttl: '0', type: 'A', content: '1.2.3' })
  ).toEqual({ ttl: 'TTL 范围为 1–86400 秒', content: '请输入有效的 IPv4 地址' })
  expect(issues('cloudflare', { ttl: '86401' })).toEqual({
    ttl: 'TTL 范围为 1–86400 秒',
  })
  expect(issues('cloudflare', { ttl: '' })).toEqual({ ttl: '请输入 TTL' })
  expect(issues('cloudflare', { ttl: '1.5' })).toEqual({
    ttl: 'TTL 必须是整数',
  })
  expect(issues('aliyun', { ttl: '300' })).toEqual({
    ttl: '阿里云 DNS 的 TTL 必须在 600 到 86400 秒之间',
  })
  expect(issues('aliyun', { type: 'AAAA', content: '192.0.2.1' })).toEqual({
    content: '请输入有效的 IPv6 地址',
  })
  expect(issues('aliyun', { type: 'MX', priority: '-1' })).toEqual({
    priority: '优先级范围为 0–65535',
  })
  expect(issues('aliyun', { type: 'MX', priority: '65536' })).toEqual({
    priority: '优先级范围为 0–65535',
  })
  expect(issues('aliyun', { line_code: '' })).toEqual({
    line_code: '请选择解析线路',
  })
})

test('record editing accepts boundary values and keeps type-specific fields only', () => {
  expect(
    recordFormSchema('cloudflare').parse({
      ...values,
      type: 'MX',
      content: 'x'.repeat(4096),
      ttl: '86400',
      priority: '65535',
      proxied: true,
    })
  ).toMatchObject({ ttl: 86400, priority: 65535, proxied: false })
  const a = recordFormSchema('cloudflare').parse({
    ...values,
    type: 'A',
    name: 'www',
    content: '192.0.2.10',
    ttl: '1',
    proxied: true,
  })
  expect(a).toEqual({
    type: 'A',
    name: 'www',
    content: '192.0.2.10',
    ttl: 1,
    proxied: true,
    line_code: 'default',
  })
  // Only Cloudflare proxies.
  expect(
    recordFormSchema('aliyun').parse({
      ...values,
      type: 'AAAA',
      content: '2001:db8::1',
      proxied: true,
    }).proxied
  ).toBe(false)
})

test('a new record starts from the provider defaults and an existing one from its values', () => {
  expect(recordFormValues('cloudflare').ttl).toBe('1')
  expect(recordFormValues('aliyun').ttl).toBe('600')
  const mx: DnsRecord = {
    id: 'mx',
    type: 'MX',
    name: '@',
    content: 'mx.example.test',
    ttl: 600,
    priority: 5,
    proxied: false,
    line_code: 'telecom',
  }
  expect(recordFormValues('aliyun', mx)).toEqual({
    type: 'MX',
    name: '',
    content: 'mx.example.test',
    ttl: '600',
    priority: '5',
    proxied: false,
    line_code: 'telecom',
  })
})

test('one add, edit or removal changes only its record in the saved set', () => {
  const record = (id: string): DnsRecord => ({
    id,
    type: 'A',
    name: id,
    content: '192.0.2.1',
    ttl: 600,
    proxied: false,
    line_code: 'default',
  })
  const records = [record('a'), record('b')]
  expect(
    applyRecordChange(records, { kind: 'add', record: record('c') }).map(
      ({ id }) => id
    )
  ).toEqual(['a', 'b', 'c'])
  const edited = applyRecordChange(records, {
    kind: 'edit',
    record: { ...record('b'), content: '192.0.2.2' },
  })
  expect(edited[0]).toBe(records[0])
  expect(edited[1].content).toBe('192.0.2.2')
  expect(applyRecordChange(records, { kind: 'remove', id: 'a' })).toEqual([
    records[1],
  ])
})
