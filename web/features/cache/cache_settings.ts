import { z } from 'zod'

export const cacheSettingDefinitions = [
  { id: 'edge', name: '节点缓存时间', english: 'Edge TTL' },
  { id: 'status', name: '按状态码设置缓存时间', english: 'Status code TTL' },
  { id: 'browser', name: '浏览器缓存时间', english: 'Browser TTL' },
  { id: 'key', name: '自定义缓存键', english: 'Cache key' },
  { id: 'armor', name: '缓存欺骗防护', english: 'Cache Deception Armor' },
  { id: 'device', name: '按设备类型区分缓存', english: 'Cache by device type' },
  { id: 'sort', name: '查询参数排序', english: 'Query String Sort' },
  {
    id: 'stale',
    name: '更新期间使用过期内容',
    english: 'Serve stale while updating',
  },
  { id: 'etag', name: '遵循强 ETag', english: 'Respect Strong ETags' },
  { id: 'origin', name: '源站缓存控制', english: 'Origin Cache Control' },
  { id: 'vary', name: 'Vary 响应头', english: 'Vary' },
  { id: 'range', name: '源站范围请求', english: 'Origin Range Requests' },
  { id: 'ports', name: '额外缓存端口', english: 'Caching on Port' },
  { id: 'timeout', name: '源站读取超时', english: 'Proxy Read Timeout' },
  {
    id: 'errors',
    name: '透传源站错误页面',
    english: 'Origin error page pass-through',
  },
] as const
export type CacheSettingId = (typeof cacheSettingDefinitions)[number]['id']
const name = z.string().min(1).max(128)
const names = z
  .array(name)
  .max(100)
  .refine((values) => new Set(values).size === values.length, '名称不能重复')
const header = name.regex(
  /^[!#$%&'*+.^_`|~0-9a-z-]+$/,
  '请求头名称请使用小写字母和合法字符'
)
const headers = z
  .array(header)
  .max(100)
  .refine((values) => new Set(values).size === values.length, '请求头不能重复')
export const cacheSettingsSchema = z
  .object({
    enabled: z
      .array(z.enum(cacheSettingDefinitions.map((value) => value.id)))
      .max(15)
      .refine(
        (values) => new Set(values).size === values.length,
        '配置项不能重复'
      ),
    status_ttl: z
      .array(
        z.object({
          from: z.number().int().min(100).max(999),
          to: z.number().int().min(100).max(999),
          seconds: z.number().int().min(-1).max(31536000),
        })
      )
      .max(32),
    key: z.object({
      query_mode: z.enum(['all', 'ignore', 'include', 'exclude']),
      query_parameters: names,
      headers,
      header_presence: headers,
      header_values: z
        .array(
          z.object({
            name: header,
            values: z.array(z.string().min(1).max(1024)).min(1).max(3),
          })
        )
        .max(50),
      cookies: names,
      cookie_presence: names,
      include_origin: z.boolean(),
      host: z.enum(['original', 'resolved']),
      country: z.boolean(),
      language: z.boolean(),
    }),
    armor: z.boolean(),
    device: z.boolean(),
    sort_query: z.boolean(),
    serve_stale: z.boolean(),
    strong_etags: z.boolean(),
    origin_control: z.boolean(),
    vary_default: z.enum(['bypass', 'passthrough', 'normalize']),
    vary_headers: z
      .array(
        z.object({
          name: header,
          action: z.enum(['normalize', 'passthrough', 'bypass']),
          values: z.array(z.string().min(1).max(128)).max(20),
        })
      )
      .max(50),
    origin_range: z.enum(['default', 'on', 'off']),
    ports: z.array(z.number().int().min(1).max(65535)).max(64),
    read_timeout: z.number().int().min(1).max(6000),
    error_passthrough: z.boolean(),
  })
  .superRefine((value, ctx) => {
    for (const [index, entry] of value.status_ttl.entries()) {
      if (entry.from > entry.to)
        ctx.addIssue({
          code: 'custom',
          path: ['status_ttl', index],
          message: '起始状态码不能大于结束状态码',
        })
      if (
        value.status_ttl
          .slice(0, index)
          .some((other) => entry.from <= other.to && entry.to >= other.from)
      )
        ctx.addIssue({
          code: 'custom',
          path: ['status_ttl', index],
          message: '状态码范围不能重叠',
        })
    }
    if (
      ['include', 'exclude'].includes(value.key.query_mode) &&
      !value.key.query_parameters.length
    )
      ctx.addIssue({
        code: 'custom',
        path: ['key', 'query_parameters'],
        message: '请填写查询参数名称',
      })
    const restrictedHeaders = [
      'accept',
      'accept-charset',
      'accept-encoding',
      'accept-datetime',
      'accept-language',
      'referer',
      'user-agent',
    ]
    for (const [index, header] of value.key.headers.entries()) {
      if (restrictedHeaders.includes(header))
        ctx.addIssue({
          code: 'custom',
          path: ['key', 'headers', index],
          message: '此请求头必须使用1至3个指定值区分缓存',
        })
    }
    const valueNames = new Set<string>()
    for (const [index, header] of value.key.header_values.entries()) {
      if (valueNames.has(header.name))
        ctx.addIssue({
          code: 'custom',
          path: ['key', 'header_values', index],
          message: '请求头不能重复',
        })
      valueNames.add(header.name)
    }
    const varyNames = new Set<string>()
    for (const [index, entry] of value.vary_headers.entries()) {
      if (
        varyNames.has(entry.name) ||
        entry.name.startsWith('cf-') ||
        entry.name.startsWith('cf_') ||
        !/^[a-z0-9_-]+$/.test(entry.name) ||
        [
          'connection',
          'host',
          'cache-control',
          'transfer-encoding',
          'keep-alive',
          'proxy-authenticate',
          'proxy-authorization',
          'te',
          'trailer',
          'upgrade',
          'cdn-cache-control',
          'surrogate-control',
        ].includes(entry.name)
      )
        ctx.addIssue({
          code: 'custom',
          path: ['vary_headers', index, 'name'],
          message: 'Vary 字段重复或不允许使用',
        })
      varyNames.add(entry.name)
      if (entry.values.some((value) => !/^[\x20-\x7e]+$/.test(value)))
        ctx.addIssue({
          code: 'custom',
          path: ['vary_headers', index, 'values'],
          message: '标准化允许值只能包含可打印 ASCII 字符',
        })
      if (entry.name === 'accept' && entry.values.length > 10)
        ctx.addIssue({
          code: 'custom',
          path: ['vary_headers', index, 'values'],
          message: 'MIME 类型最多 10 项',
        })
    }
  })
export type CacheSettings = z.infer<typeof cacheSettingsSchema>
export function newCacheSettings(): CacheSettings {
  return {
    enabled: ['edge'],
    status_ttl: [],
    key: {
      query_mode: 'all',
      query_parameters: [],
      headers: [],
      header_presence: [],
      header_values: [],
      cookies: [],
      cookie_presence: [],
      include_origin: true,
      host: 'original',
      country: false,
      language: false,
    },
    armor: false,
    device: false,
    sort_query: false,
    serve_stale: true,
    strong_etags: false,
    origin_control: true,
    vary_default: 'bypass',
    vary_headers: [],
    origin_range: 'default',
    ports: [],
    read_timeout: 120,
    error_passthrough: false,
  }
}
export function removeCacheSetting(
  settings: CacheSettings,
  id: CacheSettingId
): CacheSettings {
  const defaults = newCacheSettings()
  const next = {
    ...settings,
    enabled: settings.enabled.filter((value) => value !== id),
  }
  switch (id) {
    case 'status':
      next.status_ttl = []
      break
    case 'key':
      next.key = defaults.key
      break
    case 'armor':
      next.armor = defaults.armor
      break
    case 'device':
      next.device = defaults.device
      break
    case 'sort':
      next.sort_query = defaults.sort_query
      break
    case 'stale':
      next.serve_stale = defaults.serve_stale
      break
    case 'etag':
      next.strong_etags = defaults.strong_etags
      break
    case 'origin':
      next.origin_control = defaults.origin_control
      break
    case 'vary':
      next.vary_default = defaults.vary_default
      next.vary_headers = []
      break
    case 'range':
      next.origin_range = defaults.origin_range
      break
    case 'ports':
      next.ports = []
      break
    case 'timeout':
      next.read_timeout = defaults.read_timeout
      break
    case 'errors':
      next.error_passthrough = defaults.error_passthrough
      break
  }
  return next
}
