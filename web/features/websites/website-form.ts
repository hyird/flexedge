import { z } from 'zod'
import type { WebsiteConfig } from './types'

const domainSchema = z.object({
  id: z.string().uuid(),
  hostname: z.string().trim().min(1, '请输入域名').max(253),
  dns_mode: z.enum(['managed', 'external']),
})

const originSchema = z.object({
  id: z.string().uuid(),
  group: z.string().trim().min(1, '请输入源站组').max(100),
  protocol: z.enum(['http', 'https']),
  host: z.string().trim().min(1, '请输入源站地址').max(253),
  port: z.number().int().min(1).max(65535),
  role: z.enum(['primary', 'backup']),
  weight: z.number().int().min(1).max(100),
  status: z.enum(['enabled', 'disabled']),
})

export const routeMethods = [
  'GET',
  'HEAD',
  'POST',
  'PUT',
  'PATCH',
  'DELETE',
  'OPTIONS',
] as const

function routeHeadersToText(value: unknown) {
  if (!Array.isArray(value)) return ''
  return value
    .flatMap((header) => {
      if (!header || typeof header !== 'object') return []
      const record = header as Record<string, unknown>
      if (typeof record.name !== 'string' || typeof record.value !== 'string') {
        return []
      }
      return [`${record.name}: ${record.value}`]
    })
    .join('\n')
}

export function parseRouteHeaders(value: string) {
  return value
    .split(/\r?\n/)
    .map((line) => line.trim())
    .filter(Boolean)
    .map((line) => {
      const separator = line.indexOf(':')
      return {
        name: line.slice(0, separator).trim(),
        value: line.slice(separator + 1).trim(),
      }
    })
}

export function valuesToLines(values: string[]) {
  return values.join('\n')
}

export function linesToValues(value: string) {
  return value
    .split(/[\r\n,]+/)
    .map((item) => item.trim())
    .filter(Boolean)
}

function hasValidRouteHeaderText(value: string) {
  return value.split(/\r?\n/).every((line) => {
    const trimmed = line.trim()
    return !trimmed || trimmed.indexOf(':') > 0
  })
}

const routeRuleSchema = z
  .object({
    id: z.string().uuid(),
    status: z.enum(['enabled', 'disabled']),
    match_type: z.enum(['exact', 'prefix']),
    path: z.string().trim().min(1, '请输入匹配路径').max(2048),
    methods: z.array(z.enum(routeMethods)).max(routeMethods.length),
    action: z.enum(['proxy', 'redirect']),
    rewrite_path: z.string().trim().max(2048),
    redirect_url: z.string().trim().max(2048),
    redirect_status: z.number().int().min(0).max(302),
    origin_group: z.string().trim().max(100),
    request_headers_text: z
      .string()
      .refine(hasValidRouteHeaderText, '每行请填写为 Header: value'),
    response_headers_text: z
      .string()
      .refine(hasValidRouteHeaderText, '每行请填写为 Header: value'),
  })
  .superRefine((rule, context) => {
    if (!rule.path.startsWith('/')) {
      context.addIssue({
        code: 'custom',
        path: ['path'],
        message: '匹配路径必须以 / 开头',
      })
    }
    if (rule.action === 'proxy') {
      if (!rule.origin_group) {
        context.addIssue({
          code: 'custom',
          path: ['origin_group'],
          message: '请选择源站组',
        })
      }
      if (rule.rewrite_path && !rule.rewrite_path.startsWith('/')) {
        context.addIssue({
          code: 'custom',
          path: ['rewrite_path'],
          message: '重写路径必须以 / 开头',
        })
      }
      return
    }
    if (!rule.redirect_url || !/^(\/|https?:\/\/)/.test(rule.redirect_url)) {
      context.addIssue({
        code: 'custom',
        path: ['redirect_url'],
        message: '跳转地址需以 /、http:// 或 https:// 开头',
      })
    }
    if (rule.redirect_status !== 301 && rule.redirect_status !== 302) {
      context.addIssue({
        code: 'custom',
        path: ['redirect_status'],
        message: '请选择 301 或 302',
      })
    }
  })

export const websiteFormSchema = z.object({
  cluster_id: z.string().uuid('请选择所属集群'),
  status: z.enum(['enabled', 'disabled']),
  name: z.string().trim().min(1, '请输入网站名称').max(100),
  domains: z.array(domainSchema).min(1, '至少添加一个域名').max(100),
  origins: z.array(originSchema).min(1, '至少添加一个源站').max(100),
  default_origin_group: z.string().trim().min(1, '请选择默认源站组').max(100),
  origin_host_header: z.string().trim().min(1, '请输入回源 Host').max(253),
  pass_client_ip: z.boolean(),
  health_check_enabled: z.boolean(),
  health_check_path: z.string().trim().min(1).max(2048),
  health_check_interval_seconds: z.number().int().min(1).max(3600),
  health_check_timeout_seconds: z.number().int().min(1).max(300),
  health_check_expected_status: z.number().int().min(100).max(599),
  healthy_threshold: z.number().int().min(1).max(10),
  unhealthy_threshold: z.number().int().min(1).max(10),
  access_log_enabled: z.boolean(),
  access_log_request_headers: z.boolean(),
  access_log_request_body: z.boolean(),
  access_log_response_headers: z.boolean(),
  access_log_query_params: z.boolean(),
  access_log_cookies: z.boolean(),
  access_log_referer: z.boolean(),
  access_log_user_agent: z.boolean(),
  access_log_status_code_ranges: z.array(
    z.enum(['1xx', '2xx', '3xx', '4xx', '5xx'])
  ),
  access_log_client_abort: z.boolean(),
  https_enabled: z.boolean(),
  certificate_ids: z.array(z.string().uuid()).max(20),
  minimum_tls_version: z.enum(['1.2', '1.3']),
  force_https: z.boolean(),
  http2_enabled: z.boolean(),
  hsts_enabled: z.boolean(),
  response_compression_enabled: z.boolean(),
  response_compression_min_bytes: z.number().int().min(256).max(1048576),
  response_compression_max_bytes: z.number().int().min(0).max(67108864),
  response_compression_algorithms: z.array(z.enum(['br', 'zstd', 'gzip'])),
  response_compression_mime_types: z.array(z.string().trim()),
  response_compression_extensions: z.array(z.string().trim()),
  response_compression_excluded_extensions: z.array(z.string().trim()),
  route_rules: z.array(routeRuleSchema).max(100),
})

export type WebsiteFormValues = z.infer<typeof websiteFormSchema>
export type WebsiteRouteRuleForm = z.infer<typeof routeRuleSchema>

export function routeRuleToForm(rule: Record<string, unknown>): WebsiteRouteRuleForm {
  const methods = Array.isArray(rule.methods)
    ? rule.methods.filter((method): method is (typeof routeMethods)[number] =>
        routeMethods.includes(method as (typeof routeMethods)[number])
      )
    : []
  const action = rule.action === 'redirect' ? 'redirect' : 'proxy'

  return {
    id: typeof rule.id === 'string' ? rule.id : crypto.randomUUID(),
    status: rule.status === 'disabled' ? 'disabled' : 'enabled',
    match_type: rule.match_type === 'exact' ? 'exact' : 'prefix',
    path: typeof rule.path === 'string' ? rule.path : '/',
    methods,
    action,
    rewrite_path:
      typeof rule.rewrite_path === 'string' ? rule.rewrite_path : '',
    redirect_url:
      typeof rule.redirect_url === 'string' ? rule.redirect_url : '',
    redirect_status:
      typeof rule.redirect_status === 'number'
        ? rule.redirect_status
        : action === 'redirect'
          ? 302
          : 0,
    origin_group:
      typeof rule.origin_group === 'string' ? rule.origin_group : '',
    request_headers_text: routeHeadersToText(rule.request_headers),
    response_headers_text: routeHeadersToText(rule.response_headers),
  }
}
export function defaultWebsiteConfig(): WebsiteConfig {
  return {
    name: '',
    domains: [],
    origins: [],
    default_origin_group: 'default',
    origin_host_header: '$host',
    origin_connect_timeout_seconds: 10,
    origin_read_timeout_seconds: 30,
    pass_client_ip: true,
    health_check_enabled: true,
    health_check_path: '/',
    health_check_interval_seconds: 10,
    health_check_timeout_seconds: 3,
    health_check_expected_status: 200,
    healthy_threshold: 2,
    unhealthy_threshold: 3,
    access_log_enabled: true,
    access_log_request_headers: false,
    access_log_request_body: false,
    access_log_response_headers: false,
    access_log_query_params: true,
    access_log_cookies: false,
    access_log_referer: true,
    access_log_user_agent: true,
    access_log_status_code_ranges: ['2xx', '3xx', '4xx', '5xx'],
    access_log_client_abort: true,
    https_enabled: false,
    certificate_ids: [],
    minimum_tls_version: '1.2',
    force_https: false,
    http2_enabled: true,
    hsts_enabled: false,
    response_compression_enabled: true,
    response_compression_min_bytes: 1024,
    response_compression_max_bytes: 0,
    response_compression_algorithms: ['br', 'gzip'],
    response_compression_mime_types: [
      'text/*',
      'application/json',
      'application/javascript',
    ],
    response_compression_extensions: [],
    response_compression_excluded_extensions: [
      '.jpg',
      '.jpeg',
      '.png',
      '.gif',
      '.webp',
      '.zip',
      '.gz',
      '.mp4',
    ],
    route_rules: [],
  }
}
