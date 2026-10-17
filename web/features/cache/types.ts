import { z } from 'zod'
import { RE2JS } from 're2js'

export const cacheMatchLabels = {
  all: '全站',
  extension: '文件扩展名',
  exact: '完整路径',
  prefix: '路径前缀',
  wildcard: '通配符',
  regex: '正则表达式',
} as const
const bytes = z
  .number()
  .int()
  .nonnegative()
  .max(2 ** 50)
export const cacheRuleSchema = z
  .object({
    id: z.string().min(1).max(64),
    name: z.string().trim().min(1, '请输入规则名称').max(100),
    enabled: z.boolean(),
    match_type: z.enum([
      'all',
      'extension',
      'exact',
      'prefix',
      'wildcard',
      'regex',
    ]),
    patterns: z.array(z.string().trim().min(1).max(512)).max(64),
    action: z.enum(['cache', 'bypass']),
    ttl_mode: z.enum(['origin', 'fixed']),
    ttl_seconds: z.number().int().min(1).max(31536000),
    ignore_query: z.boolean(),
    methods: z
      .array(z.enum(['GET', 'HEAD']))
      .min(1)
      .max(2),
    status_codes: z
      .array(
        z
          .number()
          .int()
          .min(200)
          .max(599)
          .refine((v) => v !== 206 && v !== 304, '请输入完整响应状态码')
      )
      .min(1)
      .max(32),
    min_object_bytes: bytes,
    max_object_bytes: bytes,
    skip_set_cookie: z.boolean(),
    skip_no_cache: z.boolean().default(true),
    respect_request_no_cache: z.boolean().default(true),
    browser_ttl_seconds: z.number().int().min(0).max(31536000),
  })
  .superRefine((rule, ctx) => {
    const issue = (message: string) =>
      ctx.addIssue({ code: 'custom', path: ['patterns'], message })
    if (rule.match_type === 'all') {
      if (rule.patterns.length) issue('全站匹配无需填写条件')
      return
    }
    if (!rule.patterns.length) issue('请填写匹配内容')
    for (const pattern of rule.patterns) {
      if (
        [...pattern].some(
          (char) => char.charCodeAt(0) < 32 || char.charCodeAt(0) === 127
        )
      )
        issue('匹配内容不能包含控制字符')
      if (
        ['exact', 'prefix', 'wildcard'].includes(rule.match_type) &&
        !pattern.startsWith('/')
      )
        issue('路径必须以 / 开头')
      if (
        rule.match_type === 'extension' &&
        !/^\.[^/?#*\s.][^/?#*\s]*$/.test(pattern)
      )
        issue('扩展名请填写 .js、.css等格式')
      if (rule.match_type === 'regex') {
        try {
          RE2JS.compile(pattern)
        } catch {
          issue('正则表达式不正确')
        }
      }
    }
  })
export const cacheConfigSchema = z
  .object({
    storage: z.enum(['tiered', 'memory', 'disk']),
    memory_bytes: bytes,
    disk_bytes: bytes,
    memory_percent: z.number().int().min(0).max(100).default(0),
    disk_percent: z.number().int().min(0).max(100).default(0),
    memory_reserved_bytes: bytes.default(0),
    disk_reserved_bytes: bytes.default(0),
    max_object_bytes: bytes
      .min(1)
      .max(256 * 1024 ** 2, '单对象大小不能超过256MiB'),
    directory: z
      .string()
      .min(1)
      .max(128)
      .regex(
        /^[a-zA-Z0-9_-]+(?:\/[a-zA-Z0-9_-]+)*$/,
        '请输入节点数据目录内的相对路径'
      ),
    rules: z.array(cacheRuleSchema).max(128),
  })
  .superRefine((config, ctx) => {
    if (
      config.storage === 'tiered' &&
      !config.memory_percent &&
      config.memory_bytes < 1
    )
      ctx.addIssue({
        code: 'custom',
        path: ['memory_bytes'],
        message: 'L1内存容量必须大于0',
      })
    const capacity =
      config.storage === 'memory' ? config.memory_bytes : config.disk_bytes
    if (
      !(config.storage === 'memory'
        ? config.memory_percent
        : config.disk_percent) &&
      capacity < config.max_object_bytes
    )
      ctx.addIssue({
        code: 'custom',
        path: [config.storage === 'memory' ? 'memory_bytes' : 'disk_bytes'],
        message: '存储容量不能小于单对象上限',
      })
    const ids = new Set<string>()
    config.rules.forEach((rule, index) => {
      if (ids.has(rule.id))
        ctx.addIssue({
          code: 'custom',
          path: ['rules', index, 'id'],
          message: '规则标识不能重复',
        })
      ids.add(rule.id)
      if (
        rule.max_object_bytes > config.max_object_bytes ||
        rule.min_object_bytes > config.max_object_bytes ||
        (rule.max_object_bytes > 0 &&
          rule.min_object_bytes > rule.max_object_bytes)
      )
        ctx.addIssue({
          code: 'custom',
          path: ['rules', index, 'max_object_bytes'],
          message: '规则内容大小不能超过策略上限，且最大值不能小于最小值',
        })
    })
  })
export const cachePolicyInputSchema = z.object({
  name: z.string().trim().min(1, '请输入策略名称').max(100),
  description: z.string().max(1000),
  status: z.enum(['enabled', 'disabled']),
  config: cacheConfigSchema,
})
export const cachePolicySchema = cachePolicyInputSchema.extend({
  id: z.string().uuid(),
  is_builtin: z.boolean(),
  revision: z.number().int().positive(),
  website_count: z.number().int().nonnegative(),
  created_at: z.string(),
  updated_at: z.string(),
})
export type CacheRule = z.infer<typeof cacheRuleSchema>
export type CacheConfig = z.infer<typeof cacheConfigSchema>
export type CachePolicyInput = z.infer<typeof cachePolicyInputSchema>
export type CachePolicy = z.infer<typeof cachePolicySchema>
export const operationKinds = ['prewarm', 'refresh', 'clean'] as const
export type CacheOperationKind = (typeof operationKinds)[number]
export const cacheOperationInputSchema = z
  .object({
    operation: z.enum(operationKinds),
    object_id: z.string().uuid('请选择网站或缓存策略'),
    scope: z.enum(['urls', 'prefix', 'all', 'expired']),
    urls: z.array(z.string().max(4096)).max(100),
  })
  .superRefine((value, ctx) => {
    const allowed =
      value.operation === 'prewarm'
        ? ['urls']
        : value.operation === 'refresh'
          ? ['urls', 'prefix', 'all']
          : ['all', 'expired']
    if (!allowed.includes(value.scope))
      ctx.addIssue({
        code: 'custom',
        path: ['scope'],
        message: '操作范围不正确',
      })
    const needsUrls = value.scope === 'urls' || value.scope === 'prefix'
    if (needsUrls && !value.urls.length)
      ctx.addIssue({
        code: 'custom',
        path: ['urls'],
        message: '请填写1至100个URL',
      })
    if (!needsUrls && value.urls.length)
      ctx.addIssue({
        code: 'custom',
        path: ['urls'],
        message: '当前操作无需填写URL',
      })
    for (const text of value.urls) {
      try {
        const url = new URL(text)
        if (
          !['http:', 'https:'].includes(url.protocol) ||
          url.username ||
          url.password ||
          url.hash ||
          /[\s\\]/.test(text) ||
          (value.scope === 'prefix' && url.search)
        )
          throw new Error()
      } catch {
        ctx.addIssue({
          code: 'custom',
          path: ['urls'],
          message: '请填写完整的HTTP/HTTPS URL；目录不含查询参数',
        })
      }
    }
  })
export type CacheOperationInput = z.infer<typeof cacheOperationInputSchema>
export const cacheTargetSchema = z.object({
  node_id: z.string(),
  node_name: z.string(),
  status: z.string(),
  error: z.string(),
  affected_objects: z.number(),
  affected_bytes: z.number(),
  completed_urls: z.number(),
  failures: z.array(z.string()),
})
export const cacheOperationSchema = z.object({
  id: z.string(),
  operation: z.enum(operationKinds),
  object_id: z.string(),
  name: z.string(),
  scope: z.string(),
  status: z.string(),
  error: z.string(),
  updated_at: z.string(),
  targets: z.array(cacheTargetSchema),
})
export type CacheOperation = z.infer<typeof cacheOperationSchema>
export function defaultCacheConfig(): CacheConfig {
  return {
    storage: 'tiered',
    memory_bytes: 64 * 1024 ** 2,
    disk_bytes: 1024 ** 3,
    memory_percent: 50,
    disk_percent: 50,
    memory_reserved_bytes: 512 * 1024 ** 2,
    disk_reserved_bytes: 5 * 1024 ** 3,
    max_object_bytes: 64 * 1024 ** 2,
    directory: 'cache',
    rules: defaultCacheRules(),
  }
}
export function newCacheRule(): CacheRule {
  return {
    id: crypto.randomUUID(),
    name: '',
    enabled: true,
    match_type: 'extension',
    patterns: [],
    action: 'cache',
    ttl_mode: 'origin',
    ttl_seconds: 86400,
    ignore_query: false,
    methods: ['GET', 'HEAD'],
    status_codes: [200],
    min_object_bytes: 0,
    max_object_bytes: 0,
    skip_set_cookie: true,
    skip_no_cache: true,
    respect_request_no_cache: true,
    browser_ttl_seconds: 0,
  }
}
export function defaultCacheRules(): CacheRule[] {
  return [
    {
      ...newCacheRule(),
      name: '静态资源缓存一天',
      patterns: [
        '.html',
        '.css',
        '.js',
        '.mjs',
        '.jpg',
        '.jpeg',
        '.png',
        '.gif',
        '.webp',
        '.avif',
        '.svg',
        '.ico',
        '.woff',
        '.woff2',
        '.ttf',
        '.otf',
      ],
      ttl_mode: 'fixed',
      ttl_seconds: 86400,
    },
  ]
}
