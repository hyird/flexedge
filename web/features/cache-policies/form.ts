import { z } from 'zod'
import { DEFAULT_CACHE_RULE_ID } from './utils'

export const MAX_SAFE_INTEGER = Number.MAX_SAFE_INTEGER
export const CACHE_STATUS_CODES = [200, 301, 302, 404] as const

export const DURATION_UNITS = [
  { value: 'seconds', label: '秒', factor: 1 },
  { value: 'minutes', label: '分钟', factor: 60 },
  { value: 'hours', label: '小时', factor: 60 * 60 },
  { value: 'days', label: '天', factor: 60 * 60 * 24 },
] as const

export const BYTE_UNITS = [
  { value: 'bytes', label: 'B', factor: 1, integerBase: true },
  { value: 'kibibytes', label: 'KiB', factor: 1024, integerBase: true },
  { value: 'mebibytes', label: 'MiB', factor: 1024 ** 2, integerBase: true },
  { value: 'gibibytes', label: 'GiB', factor: 1024 ** 3, integerBase: true },
] as const

export type DurationUnit = (typeof DURATION_UNITS)[number]['value']
export type ByteUnit = (typeof BYTE_UNITS)[number]['value']

type UnitDefinition = { factor: number; integerBase?: boolean }

export function utf8ByteLength(value: string) {
  return new TextEncoder().encode(value).byteLength
}

function validPattern(matchType: string, pattern: string) {
  if (!pattern) return false
  for (const character of pattern) {
    const code = character.charCodeAt(0)
    if (code <= 32 || code === 127) return false
  }
  if (matchType === 'prefix' || matchType === 'exact') {
    return (
      pattern.startsWith('/') &&
      utf8ByteLength(pattern) <= 2048 &&
      !pattern.includes('?') &&
      !pattern.includes('#')
    )
  }
  if (matchType === 'extension') {
    return (
      pattern.startsWith('.') &&
      utf8ByteLength(pattern) >= 2 &&
      utf8ByteLength(pattern) <= 32 &&
      /^\.[A-Za-z0-9._-]+$/u.test(pattern)
    )
  }
  return false
}

export function unitValueToBase(
  rawValue: string | number,
  unit: UnitDefinition,
  max: number
) {
  const raw =
    typeof rawValue === 'number'
      ? rawValue
      : rawValue.trim() === ''
        ? Number.NaN
        : Number(rawValue)
  if (!Number.isFinite(raw) || raw < 0) return undefined
  const base = raw * unit.factor
  if (!Number.isFinite(base) || base < 0 || base > max) return undefined
  if (unit.integerBase && !Number.isSafeInteger(base)) return undefined
  const rounded = Math.round(base)
  const tolerance = Number.EPSILON * Math.max(1, Math.abs(base)) * 4
  if (Math.abs(base - rounded) > tolerance) return undefined
  return Number.isSafeInteger(rounded) ? rounded : undefined
}

export function formatUnitValue(value: number, unit: UnitDefinition) {
  if (!Number.isFinite(value)) return ''
  return String(value / unit.factor)
}

export function defaultDurationUnit(value: number): DurationUnit {
  const unit = [...DURATION_UNITS]
    .reverse()
    .find((candidate) => value > 0 && value % candidate.factor === 0)
  return unit?.value ?? 'seconds'
}

export function defaultByteUnit(value: number): ByteUnit {
  const unit = [...BYTE_UNITS]
    .reverse()
    .find((candidate) => value > 0 && value % candidate.factor === 0)
  return unit?.value ?? 'bytes'
}

export function formatDuration(seconds: number) {
  if (seconds === 0) return '0 秒'
  const unit = [...DURATION_UNITS]
    .reverse()
    .find((candidate) => seconds % candidate.factor === 0)
  if (!unit) return `${seconds} 秒`
  return `${seconds / unit.factor} ${unit.label}`
}

export function formatBytesValue(bytes: number) {
  if (bytes === 0) return '节点对象上限'
  const unit = defaultByteUnit(bytes)
  const definition = BYTE_UNITS.find((candidate) => candidate.value === unit)
  return `${formatUnitValue(bytes, definition ?? BYTE_UNITS[0])} ${definition?.label ?? 'B'}`
}

const statusCodeSchema = z
  .number({ error: '请输入状态码' })
  .int('状态码必须是整数')
  .refine(
    (value) => (CACHE_STATUS_CODES as readonly number[]).includes(value),
    '仅支持 200、301、302、404'
  )

export const cacheRuleSchema = z
  .object({
    id: z.string().uuid('规则标识无效'),
    name: z
      .string()
      .trim()
      .min(1, '请输入规则名称')
      .superRefine((value, context) => {
        if (utf8ByteLength(value) > 100)
          context.addIssue({
            code: 'custom',
            message: '规则名称最多 100 字节',
          })
      }),
    status: z.enum(['enabled', 'disabled']),
    match_type: z.enum(['all', 'prefix', 'exact', 'extension']),
    patterns: z.array(z.string()).max(64, '最多添加 64 个匹配项'),
    action: z.enum(['cache', 'bypass']),
    ttl_seconds: z
      .number({ error: '请输入缓存 TTL' })
      .int('缓存 TTL 必须是整数秒')
      .min(0, '缓存 TTL 不能为负数')
      .max(31536000, '缓存 TTL 不能超过 365 天'),
    query_mode: z.enum(['include', 'ignore']),
    status_codes: z
      .array(statusCodeSchema)
      .min(1, '至少选择一个可缓存状态码')
      .superRefine((values, context) => {
        if (new Set(values).size !== values.length)
          context.addIssue({
            code: 'custom',
            message: '状态码不能重复',
          })
      }),
    min_object_bytes: z
      .number({ error: '请输入最小对象大小' })
      .int('最小对象大小必须是整数')
      .min(0, '最小对象大小不能为负数')
      .max(MAX_SAFE_INTEGER, '最小对象大小超过安全整数上限'),
    max_object_bytes: z
      .number({ error: '请输入最大对象大小' })
      .int('最大对象大小必须是整数')
      .min(0, '最大对象大小不能为负数')
      .max(MAX_SAFE_INTEGER, '最大对象大小超过安全整数上限'),
    stale_if_error_seconds: z
      .number({ error: '请输入错误陈旧窗口' })
      .int('错误陈旧窗口必须是整数秒')
      .min(0, '错误陈旧窗口不能为负数')
      .max(86400, '错误陈旧窗口不能超过 24 小时'),
    ignore_origin_cache_control: z.boolean(),
    range_enabled: z.boolean(),
  })
  .superRefine((rule, context) => {
    if (rule.match_type === 'all') {
      if (rule.patterns.length)
        context.addIssue({
          code: 'custom',
          path: ['patterns'],
          message: '全部请求不需要匹配项',
        })
    } else {
      if (!rule.patterns.length)
        context.addIssue({
          code: 'custom',
          path: ['patterns'],
          message: '请输入匹配项',
        })
      const seen = new Set<string>()
      for (const pattern of rule.patterns) {
        if (!validPattern(rule.match_type, pattern))
          context.addIssue({
            code: 'custom',
            path: ['patterns'],
            message:
              rule.match_type === 'extension'
                ? '扩展名须以 . 开头，只能包含字母、数字、点、下划线和连字符，且不超过 32 字节'
                : '路径须以 / 开头，不能包含查询符号或片段符号，且不超过 2048 字节',
          })
        if (seen.has(pattern))
          context.addIssue({
            code: 'custom',
            path: ['patterns'],
            message: '匹配项不能重复',
          })
        seen.add(pattern)
      }
    }
    if (
      rule.max_object_bytes > 0 &&
      rule.min_object_bytes > rule.max_object_bytes
    )
      context.addIssue({
        code: 'custom',
        path: ['max_object_bytes'],
        message: '最大对象大小必须大于或等于最小对象大小',
      })
  })

export const cachePolicySchema = z
  .object({
    name: z
      .string()
      .trim()
      .min(1, '请输入策略名称')
      .superRefine((value, context) => {
        if (utf8ByteLength(value) > 200)
          context.addIssue({
            code: 'custom',
            message: '策略名称最多 200 字节',
          })
      }),
    description: z.string().superRefine((value, context) => {
      if (utf8ByteLength(value) > 2000)
        context.addIssue({
          code: 'custom',
          message: '描述最多 2000 字节',
        })
    }),
    status: z.enum(['enabled', 'disabled']),
    rules: z.array(cacheRuleSchema).max(65, '最多添加 65 条规则'),
  })
  .superRefine((policy, context) => {
    const defaultIndexes = policy.rules
      .map((rule, index) =>
        rule.id.toLowerCase() === DEFAULT_CACHE_RULE_ID ? index : -1
      )
      .filter((index) => index >= 0)
    if (
      defaultIndexes.length !== 1 ||
      defaultIndexes[0] !== policy.rules.length - 1
    ) {
      context.addIssue({
        code: 'custom',
        path: ['rules'],
        message: '必须保留列表末尾的默认规则',
      })
      return
    }
    const defaultRule = policy.rules[defaultIndexes[0]]
    if (
      defaultRule.status !== 'enabled' ||
      defaultRule.match_type !== 'all' ||
      defaultRule.patterns.length !== 0 ||
      defaultRule.action !== 'cache'
    ) {
      context.addIssue({
        code: 'custom',
        path: ['rules', defaultIndexes[0]],
        message: '默认规则的匹配结构不可修改',
      })
    }
  })

export type CacheRuleFormValues = z.infer<typeof cacheRuleSchema>
export type CachePolicyFormValues = z.infer<typeof cachePolicySchema>
