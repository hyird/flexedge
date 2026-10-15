import { z } from 'zod'
import type { DnsRecord } from './types'

export const recordTypes: DnsRecord['type'][] = [
  'A',
  'AAAA',
  'CNAME',
  'TXT',
  'MX',
]
export const maxRecords = 10000
const aliyunMinTtl = 600

/** Inputs of one record in the inline record form; TTL and priority are kept
 * as typed text so a cleared field stays empty instead of `NaN`. */
export type RecordFormValues = {
  type: DnsRecord['type']
  name: string
  content: string
  ttl: string
  priority: string
  proxied: boolean
  line_code: string
}

/** A validated record of the form, before it gets its id. */
export type RecordInput = Omit<DnsRecord, 'id'>

/** The first problem of an integer field typed as text, if any. */
function integerIssue(
  text: string,
  min: number,
  max: number,
  messages: { required: string; integer: string; range: string }
) {
  const value = text.trim()
  if (!value) return messages.required
  if (!/^-?\d+$/.test(value)) return messages.integer
  if (Number(value) < min || Number(value) > max) return messages.range
}

/** One record of the inline form. Every field reports its first problem at
 * once; `provider` adds the provider's own limits (Aliyun TTL ≥ 600, as the
 * backend checks). Parses to the record without its id. */
export function recordFormSchema(provider: string) {
  return z
    .object({
      type: z.enum(['A', 'AAAA', 'CNAME', 'TXT', 'MX']),
      name: z.string(),
      content: z.string(),
      ttl: z.string(),
      priority: z.string(),
      proxied: z.boolean(),
      line_code: z.string(),
    })
    .superRefine((values, ctx) => {
      const issue = (field: keyof RecordFormValues, message?: string) => {
        if (message) ctx.addIssue({ code: 'custom', path: [field], message })
      }
      // Empty or `@` is the zone apex.
      issue(
        'name',
        values.name.trim().length > 253 ? '主机记录最多 253 个字符' : undefined
      )
      const content = values.content.trim()
      issue(
        'content',
        !values.content
          ? '请输入记录值'
          : values.content.length > 4096
            ? '记录值最多 4096 个字符'
            : values.type === 'A' && !z.ipv4().safeParse(content).success
              ? '请输入有效的 IPv4 地址'
              : values.type === 'AAAA' && !z.ipv6().safeParse(content).success
                ? '请输入有效的 IPv6 地址'
                : undefined
      )
      issue(
        'ttl',
        provider === 'aliyun'
          ? integerIssue(values.ttl, aliyunMinTtl, 86400, {
              required: '请输入 TTL',
              integer: 'TTL 必须是整数',
              range: '阿里云 DNS 的 TTL 必须在 600 到 86400 秒之间',
            })
          : integerIssue(values.ttl, 1, 86400, {
              required: '请输入 TTL',
              integer: 'TTL 必须是整数',
              range: 'TTL 范围为 1–86400 秒',
            })
      )
      if (values.type === 'MX')
        issue(
          'priority',
          integerIssue(values.priority, 0, 65535, {
            required: '请输入优先级',
            integer: '优先级必须是整数',
            range: '优先级范围为 0–65535',
          })
        )
      issue(
        'line_code',
        !values.line_code.trim() ? '请选择解析线路' : undefined
      )
    })
    .transform((values): RecordInput => ({
      type: values.type,
      name: values.name.trim() || '@',
      content: values.content,
      ttl: Number(values.ttl),
      ...(values.type === 'MX' ? { priority: Number(values.priority) } : {}),
      proxied:
        provider === 'cloudflare' &&
        values.type !== 'TXT' &&
        values.type !== 'MX' &&
        values.proxied,
      line_code: values.line_code.trim(),
    }))
}

/** Form inputs for `record`, or a new record's provider defaults (Cloudflare
 * TTL 1 = 自动; Aliyun's minimum 600). */
export function recordFormValues(
  provider: string,
  record?: DnsRecord
): RecordFormValues {
  if (!record)
    return {
      type: 'A',
      name: '',
      content: '',
      ttl: provider === 'cloudflare' ? '1' : String(aliyunMinTtl),
      priority: '10',
      proxied: false,
      line_code: 'default',
    }
  return {
    type: record.type,
    name: record.name === '@' ? '' : record.name,
    content: record.content,
    ttl: String(record.ttl),
    priority: String(record.priority ?? 10),
    proxied: record.proxied,
    line_code: record.line_code,
  }
}

/** One add, edit or removal applied to the zone's custom records; the whole
 * set is what `PUT /dns-zones/:id` accepts. */
export type RecordChange =
  | { kind: 'add'; record: DnsRecord }
  | { kind: 'edit'; record: DnsRecord }
  | { kind: 'remove'; id: string }

export function applyRecordChange(
  records: DnsRecord[],
  change: RecordChange
): DnsRecord[] {
  if (change.kind === 'add') return [...records, change.record]
  if (change.kind === 'remove')
    return records.filter((record) => record.id !== change.id)
  return records.map((record) =>
    record.id === change.record.id ? change.record : record
  )
}
