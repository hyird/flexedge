import { z } from 'zod'
import { certificateMatchesHostnames } from '@/features/certificates/certificate-coverage'
import {
  hostedPrefixError,
  isWebsiteHostname,
  splitHostedDomain,
  type HostedZone,
} from './hosted-domain'
import {
  compileRoutePattern,
  validCaptureTemplate,
  validRouteCondition,
} from './route-matching'
import {
  routeMethods,
  type Website,
  type WebsiteConfig,
  type WebsiteRouteRule,
} from './types'

const routeConditionSchema = z
  .object({
    source: z.enum(['header', 'query']),
    name: z.string().trim().min(1, '请输入条件名称'),
    op: z.enum(['equals', 'not_equals', 'exists', 'absent']),
    value: z.string(),
  })
  .superRefine((condition, context) => {
    if (!validRouteCondition(condition))
      context.addIssue({
        code: 'custom',
        path: ['name'],
        message: '条件名称或值不正确；存在/不存在操作无需填写值',
      })
  })

const domainSchema = z.object({
  id: z.string().uuid(),
  hostname: z
    .string()
    .trim()
    .min(1, '请输入域名')
    .max(253)
    .refine((hostname) => !hostname || isWebsiteHostname(hostname), {
      message: '域名格式不正确',
    }),
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

/** Headers each route rule may set per direction (`max_route_headers`,
 * node/proto/website_validation.h); the messages match the save check in
 * service/features/website_config/validation.h. */
const maxRouteHeaders = 20

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
    name: z
      .string()
      .trim()
      .refine(
        (value) =>
          new TextEncoder().encode(value).length <= 100 &&
          !Array.from(value).some(
            (char) => char.charCodeAt(0) < 32 || char.charCodeAt(0) === 127
          ),
        '名称最多100字节且不能包含控制字符'
      )
      .default(''),
    description: z
      .string()
      .refine(
        (value) => new TextEncoder().encode(value).length <= 1000,
        '备注最多1000字节'
      )
      .default(''),
    hostnames: z
      .array(z.string())
      .transform((items) =>
        items
          .map((item) => item.trim().toLowerCase().replace(/\.$/, ''))
          .filter(Boolean)
      )
      .pipe(
        z
          .array(
            z
              .string()
              .max(253)
              .refine(
                (value) =>
                  value
                    .split('.')
                    .every((part) =>
                      /^(?:[a-z0-9]|[a-z0-9][a-z0-9-]{0,61}[a-z0-9])$/.test(
                        part
                      )
                    ),
                '请输入精确域名，不含协议、端口或通配符'
              )
          )
          .max(100)
      )
      .refine((items) => new Set(items).size === items.length, '域名不能重复')
      .default([]),
    rewrite_mode: z.enum([
      'none',
      'replace_path',
      'strip_prefix',
      'replace_prefix',
    ]),
    query_mode: z.enum(['preserve', 'drop', 'replace']).default('preserve'),
    query_string: z
      .string()
      .refine(
        (value) =>
          new TextEncoder().encode(value).length <= 2048 &&
          !value.startsWith('?') &&
          !Array.from(value).some(
            (char) =>
              char.charCodeAt(0) <= 32 ||
              char.charCodeAt(0) === 127 ||
              char === '#'
          ),
        '参数不能含开头问号、空白或 #，最多2048字节，特殊字符请编码'
      )
      .default(''),
    status: z.enum(['enabled', 'disabled']),
    conditions: z
      .array(routeConditionSchema)
      .max(20, '最多20个条件')
      .default([]),
    match_type: z.enum(['exact', 'prefix', 'suffix', 'regex']),
    path: z.string().trim().min(1, '请输入匹配路径').max(2048),
    methods: z.array(z.enum(routeMethods)).max(routeMethods.length),
    action: z.enum(['proxy', 'redirect', 'rewrite']),
    rewrite_path: z.string().trim().max(2048),
    redirect_url: z.string().trim().max(2048),
    redirect_status: z.number().int().min(0).max(308),
    origin_group: z.string().trim().max(100),
    request_headers_text: z
      .string()
      .refine(hasValidRouteHeaderText, '每行请填写为Header: value')
      .refine(
        (value) => parseRouteHeaders(value).length <= maxRouteHeaders,
        `最多${maxRouteHeaders}个请求头`
      ),
    response_headers_text: z
      .string()
      .refine(hasValidRouteHeaderText, '每行请填写为Header: value')
      .refine(
        (value) => parseRouteHeaders(value).length <= maxRouteHeaders,
        `最多${maxRouteHeaders}个响应头`
      ),
  })
  .superRefine((rule, context) => {
    if (
      rule.action === 'rewrite' &&
      (rule.origin_group ||
        rule.redirect_url ||
        rule.redirect_status ||
        rule.request_headers_text ||
        rule.response_headers_text)
    ) {
      context.addIssue({
        code: 'custom',
        path: ['action'],
        message: '重写规则只处理路径和查询参数，不能设置源站、跳转或请求头',
      })
    }
    if (rule.match_type === 'regex') {
      try {
        const pattern = compileRoutePattern(rule.path)
        const destination =
          rule.action === 'redirect' ? rule.redirect_url : rule.rewrite_path
        if (!validCaptureTemplate(destination, pattern.groupCount()))
          context.addIssue({
            code: 'custom',
            path: [
              rule.action === 'redirect' ? 'redirect_url' : 'rewrite_path',
            ],
            message: '仅支持存在的 ${0} 到 ${9} 捕获组，字面美元符用 $$',
          })
      } catch {
        context.addIssue({
          code: 'custom',
          path: ['path'],
          message:
            '正则需符合RE2语法，最多512字节、9个捕获组；不支持前后查找或回溯引用',
        })
      }
    } else if (rule.match_type === 'suffix' && /[\s?#]/.test(rule.path)) {
      context.addIssue({
        code: 'custom',
        path: ['path'],
        message: '后缀不能含空白、查询参数或片段',
      })
    }
    if (rule.query_mode !== 'replace' && rule.query_string)
      context.addIssue({
        code: 'custom',
        path: ['query_string'],
        message: '仅替换策略可填写查询参数',
      })
    if (
      (rule.rewrite_mode === 'none' || rule.rewrite_mode === 'strip_prefix') &&
      rule.rewrite_path
    )
      context.addIssue({
        code: 'custom',
        path: ['rewrite_path'],
        message: '当前方式无需填写替换路径',
      })
    if (
      (rule.rewrite_mode === 'replace_path' ||
        rule.rewrite_mode === 'replace_prefix') &&
      !rule.rewrite_path
    )
      context.addIssue({
        code: 'custom',
        path: ['rewrite_path'],
        message: '请输入替换路径',
      })
    if (
      rule.rewrite_mode === 'strip_prefix' ||
      rule.rewrite_mode === 'replace_prefix'
    ) {
      if (rule.match_type !== 'prefix' || /[\s?#]/.test(rule.path))
        context.addIssue({
          code: 'custom',
          path: ['rewrite_mode'],
          message: '前缀重写需使用前缀匹配，路径不能含参数或片段',
        })
      if (
        rule.rewrite_mode === 'replace_prefix' &&
        /[\s?#]/.test(rule.rewrite_path)
      )
        context.addIssue({
          code: 'custom',
          path: ['rewrite_path'],
          message: '替换前缀不能含参数、片段或空白',
        })
    }
    if (rule.action === 'redirect' && rule.rewrite_mode !== 'none')
      context.addIssue({
        code: 'custom',
        path: ['rewrite_mode'],
        message: '跳转规则不能同时重写回源路径',
      })
    if (
      ['exact', 'prefix'].includes(rule.match_type) &&
      !rule.path.startsWith('/')
    ) {
      context.addIssue({
        code: 'custom',
        path: ['path'],
        message: '匹配路径必须以 / 开头',
      })
    }
    if (rule.action !== 'redirect') {
      if (rule.action === 'proxy' && !rule.origin_group) {
        context.addIssue({
          code: 'custom',
          path: ['origin_group'],
          message: '请选择源站组',
        })
      }
      if (
        rule.rewrite_path &&
        (!rule.rewrite_path.startsWith('/') || /[\s?#]/.test(rule.rewrite_path))
      ) {
        context.addIssue({
          code: 'custom',
          path: ['rewrite_path'],
          message:
            '重写路径必须以 / 开头，不能含空白、参数或片段；参数请使用查询参数策略',
        })
      }
      return
    }
    if (!rule.redirect_url || !/^(\/|https?:\/\/)/.test(rule.redirect_url)) {
      context.addIssue({
        code: 'custom',
        path: ['redirect_url'],
        message: '跳转地址需以 /、http:// 或https:// 开头',
      })
    }
    if (![301, 302, 307, 308].includes(rule.redirect_status)) {
      context.addIssue({
        code: 'custom',
        path: ['redirect_status'],
        message: '请选择 301、302、307 或 308',
      })
    }
  })

export const websiteFormSchema = z
  .object({
    cache_enabled: z.boolean().default(false),
    cache_policy_id: z
      .union([z.literal(''), z.string().uuid('请选择缓存策略')])
      .default(''),
    cluster_id: z.string().uuid('请选择所属集群'),
    status: z.enum(['enabled', 'disabled']),
    name: z.string().trim().min(1, '请输入网站名称').max(100),
    domains: z.array(domainSchema).min(1, '至少添加一个域名').max(100),
    origins: z.array(originSchema).min(1, '至少添加一个源站').max(100),
    default_origin_group: z.string().trim().min(1, '请选择默认源站组').max(100),
    origin_host_header: z.string().trim().min(1, '请输入回源Host').max(253),
    origin_http_version: z.enum(['auto', 'http1', 'http2']).default('auto'),
    origin_connect_timeout_seconds: z.number().int().min(1).max(300),
    origin_read_timeout_seconds: z.number().int().min(1).max(600),
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
    hsts_enabled: z.boolean(),
    response_compression_enabled: z.boolean(),
    response_compression_min_bytes: z.number().int().min(256).max(1048576),
    response_compression_max_bytes: z.number().int().min(0).max(67108864),
    response_compression_algorithms: z
      .array(z.enum(['br', 'zstd', 'gzip']))
      .refine(
        (items) => new Set(items).size === items.length,
        '压缩算法不能重复'
      ),
    response_compression_mime_types: z.array(z.string().trim()),
    response_compression_extensions: z.array(z.string().trim()),
    response_compression_excluded_extensions: z.array(z.string().trim()),
    route_rules: z.array(routeRuleSchema).max(100),
  })
  .superRefine((values, context) => {
    if (values.cache_enabled && !values.cache_policy_id) {
      context.addIssue({
        code: 'custom',
        path: ['cache_policy_id'],
        message: '启用缓存必须选择缓存策略',
      })
    }
    if (values.https_enabled && values.certificate_ids.length === 0) {
      context.addIssue({
        code: 'custom',
        path: ['certificate_ids'],
        message: '启用HTTPS必须绑定至少一张可用证书',
      })
    }
    if (!values.https_enabled && values.certificate_ids.length > 0) {
      context.addIssue({
        code: 'custom',
        path: ['certificate_ids'],
        message: '关闭HTTPS后不能保留证书绑定',
      })
    }
    if (
      values.response_compression_enabled &&
      !values.response_compression_algorithms.length
    ) {
      context.addIssue({
        code: 'custom',
        path: ['response_compression_algorithms'],
        message: '启用压缩时至少选择一种算法',
      })
    }
    if (
      values.response_compression_max_bytes !== 0 &&
      values.response_compression_max_bytes <
        values.response_compression_min_bytes
    ) {
      context.addIssue({
        code: 'custom',
        path: ['response_compression_max_bytes'],
        message: '最大响应大小不能小于最小响应大小',
      })
    }
  })

export type WebsiteFormValues = z.infer<typeof websiteFormSchema>
export type WebsiteRouteRuleForm = z.infer<typeof routeRuleSchema>

const zhCNErrors = z.locales.zhCN().localeError

/** Managed-domain issues that need the hosted zone list: the hostname must
 * fall inside a hosted zone (`domains.<i>.zone`) and the prefix in front of
 * that zone must be a valid host prefix (`domains.<i>.hostname`). */
function hostedDomainIssues(values: unknown, zones: readonly HostedZone[]) {
  const issues: z.core.$ZodIssue[] = []
  const domains = (values as { domains?: unknown } | null)?.domains
  if (!Array.isArray(domains)) return issues
  for (const [index, domain] of (
    domains as Partial<WebsiteFormValues['domains'][number]>[]
  ).entries()) {
    if (domain?.dns_mode !== 'managed') continue
    const hostname = domain.hostname?.trim() ?? ''
    const split = splitHostedDomain(hostname, zones)
    if (!split)
      issues.push({
        code: 'custom',
        path: ['domains', index, 'zone'],
        message: '请选择托管域名',
        input: hostname,
      })
    // Without a zone the row keeps everything it has in the prefix.
    const prefixError = hostedPrefixError(split ? split.prefix : hostname)
    if (prefixError)
      issues.push({
        code: 'custom',
        path: ['domains', index, 'hostname'],
        message: prefixError,
        input: hostname,
      })
  }
  return issues
}

/** Hostnames currently bound in the draft (blank rows skipped). */
export function websiteHostnames(values: unknown) {
  const domains = (values as { domains?: unknown } | null)?.domains
  if (!Array.isArray(domains)) return []
  return (domains as Partial<WebsiteFormValues['domains'][number]>[])
    .map((domain) => domain?.hostname?.trim() ?? '')
    .filter(Boolean)
}

type CertificateNames = { id: string; domains: readonly string[] }

/** A bound certificate must match at least one bound hostname (mirrors the
 * backend save check); hostnames left uncovered only stay on HTTP. */
function unmatchedCertificateIssues(
  values: unknown,
  certificates: readonly CertificateNames[]
) {
  const ids = (values as { certificate_ids?: unknown } | null)?.certificate_ids
  if (!Array.isArray(ids)) return []
  const hostnames = websiteHostnames(values)
  // Options and attached bindings can describe the same certificate.
  const unmatched = certificates.filter(
    (certificate, index) =>
      certificates.findIndex((other) => other.id === certificate.id) ===
        index &&
      ids.includes(certificate.id) &&
      certificate.domains.length > 0 &&
      !certificateMatchesHostnames(certificate.domains, hostnames)
  )
  if (!unmatched.length) return []
  return [
    {
      code: 'custom',
      path: ['certificate_ids'],
      message: `证书未匹配网站域名：${unmatched.map((certificate) => certificate.domains[0]).join('、')}`,
      input: ids,
    } satisfies z.core.$ZodIssue,
  ]
}

/** Validates a settings draft; built-in range and type messages are Chinese,
 * schema-specific messages take precedence. With the hosted zone list,
 * managed domains are also checked against their zone and prefix; those
 * messages replace the full-hostname format message of the same domain.
 * With known certificate names, every bound certificate must match at least
 * one bound hostname. */
export function validateWebsiteForm(
  values: unknown,
  hostedZones?: readonly HostedZone[],
  certificates?: readonly CertificateNames[]
): z.ZodSafeParseResult<WebsiteFormValues> {
  const result = websiteFormSchema.safeParse(values, { error: zhCNErrors })
  const hostedIssues = hostedZones
    ? hostedDomainIssues(values, hostedZones)
    : []
  const certificateIssues = certificates
    ? unmatchedCertificateIssues(values, certificates)
    : []
  if (!hostedIssues.length && !certificateIssues.length) return result
  const replacedHostnames = new Set(
    hostedIssues.map((issue) => `domains.${String(issue.path[1])}.hostname`)
  )
  const schemaIssues = result.success
    ? []
    : result.error.issues.filter(
        (issue) => !replacedHostnames.has(issue.path.map(String).join('.'))
      )
  return {
    success: false,
    error: new z.ZodError([
      ...hostedIssues,
      ...schemaIssues,
      ...certificateIssues,
    ]) as z.ZodError<WebsiteFormValues>,
  }
}

export function setWebsiteHttpsEnabled(
  values: WebsiteFormValues,
  enabled: boolean
): WebsiteFormValues {
  return enabled
    ? { ...values, https_enabled: true }
    : {
        ...values,
        https_enabled: false,
        certificate_ids: [],
        force_https: false,
        hsts_enabled: false,
      }
}

export type WebsiteCertificateOption = Website['certificates'][number] & {
  /** Covers at least one bound hostname. */
  matched: boolean
}

/** Usable certificates matching a bound hostname, plus every selected
 * certificate (kept visible, flagged, so it can be removed explicitly). */
export function websiteCertificateOptions(
  available: readonly { id: string; domains: string[] }[],
  selectedIds: readonly string[],
  attached: readonly Website['certificates'][number][],
  hostnames: readonly string[]
): WebsiteCertificateOption[] {
  const options: WebsiteCertificateOption[] = []
  for (const certificate of available) {
    const matched = certificateMatchesHostnames(certificate.domains, hostnames)
    if (matched || selectedIds.includes(certificate.id))
      options.push({ ...certificate, usable: true, matched })
  }
  for (const id of selectedIds) {
    if (available.some((certificate) => certificate.id === id)) continue
    const domains =
      attached.find((certificate) => certificate.id === id)?.domains ?? []
    options.push({
      id,
      domains,
      usable: false,
      matched: certificateMatchesHostnames(domains, hostnames),
    })
  }
  return options
}

/** Editor row of a stored rule; a new row passes only what it presets and
 * takes the defaults. An empty stored rewrite or query mode keeps its
 * backend meaning (`replace_path` with a rewrite path, else `none`;
 * `preserve`). */
export function routeRuleToForm(
  rule: Partial<WebsiteRouteRule>
): WebsiteRouteRuleForm {
  const action = rule.action ?? 'proxy'
  return {
    id: rule.id ?? crypto.randomUUID(),
    name: rule.name ?? '',
    description: rule.description ?? '',
    hostnames: rule.hostnames ?? [],
    rewrite_mode:
      rule.rewrite_mode || (rule.rewrite_path ? 'replace_path' : 'none'),
    query_mode: rule.query_mode || 'preserve',
    query_string: rule.query_string ?? '',
    status: rule.status ?? 'enabled',
    conditions: rule.conditions ?? [],
    match_type: rule.match_type ?? 'prefix',
    path: rule.path ?? '/',
    methods: rule.methods ?? [],
    action,
    rewrite_path: rule.rewrite_path ?? '',
    redirect_url: rule.redirect_url ?? '',
    redirect_status: rule.redirect_status ?? (action === 'redirect' ? 302 : 0),
    origin_group: rule.origin_group ?? '',
    request_headers_text: (rule.request_headers ?? [])
      .map((header) => `${header.name}: ${header.value}`)
      .join('\n'),
    response_headers_text: (rule.response_headers ?? [])
      .map((header) => `${header.name}: ${header.value}`)
      .join('\n'),
  }
}
export function defaultWebsiteConfig(): WebsiteConfig {
  return {
    cache_enabled: false,
    cache_policy_id: '',
    name: '',
    domains: [],
    origins: [],
    default_origin_group: 'default',
    origin_host_header: '$host',
    origin_http_version: 'auto',
    origin_connect_timeout_seconds: 10,
    origin_read_timeout_seconds: 30,
    pass_client_ip: true,
    health_check_enabled: true,
    health_check_path: '/',
    health_check_interval_seconds: 60,
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
