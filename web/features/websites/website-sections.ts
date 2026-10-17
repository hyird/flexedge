import type { Website } from './types'
import type { WebsiteFormValues } from './website-form'

/** Settings editor groups in navigation order; the selected group is
 * component state (an arrival may request one via `state.website.section`). */
export const websiteSections = [
  {
    id: 'basic',
    label: '基本',
    description: '网站名称、所属集群与启停状态。',
  },
  {
    id: 'domains',
    label: '域名',
    description: '网站接入域名与解析方式。',
  },
  {
    id: 'https',
    label: 'HTTPS',
    description: '证书绑定、TLS版本与HTTPS跳转。',
  },
  {
    id: 'origins',
    label: '源站',
    description: '源站组、主备角色与权重。',
  },
  {
    id: 'origin-settings',
    label: '回源',
    description: '回源Host、超时与客户端地址传递。',
  },
  {
    id: 'health',
    label: '健康检查',
    description: '边缘节点对源站的主动健康检查。',
  },
  {
    id: 'routes',
    label: '路由规则',
    description: 'URL跳转、URL重写与按规则选择源站。',
  },
  {
    id: 'cache',
    label: '缓存',
    description: '启用缓存并选择缓存策略。',
  },
  { id: 'security', label: '安全', description: '选择网站应用的安全规则集。' },
  {
    id: 'compression',
    label: '压缩',
    description: '响应压缩算法与适用内容。',
  },
  {
    id: 'logs',
    label: '日志',
    description: '访问日志的记录范围。',
  },
] as const

export type WebsiteSectionId = (typeof websiteSections)[number]['id']

export const websiteSectionIds = websiteSections.map(
  (section) => section.id
) as [WebsiteSectionId, ...WebsiteSectionId[]]

export const defaultWebsiteSection: WebsiteSectionId = 'basic'

/** Group requested by an arrival state; values read back from history may be
 * stale, so unknown ids fall back to the default group. */
export function websiteSectionFrom(id: string | undefined): WebsiteSectionId {
  return (websiteSectionIds as readonly string[]).includes(id ?? '')
    ? (id as WebsiteSectionId)
    : defaultWebsiteSection
}

/** Every form field belongs to exactly one group; the record type keeps the
 * mapping exhaustive when the form schema gains a field. */
const fieldSections: Record<keyof WebsiteFormValues, WebsiteSectionId> = {
  cache_enabled: 'cache',
  security_policy_id: 'security',
  cache_policy_id: 'cache',
  name: 'basic',
  cluster_id: 'basic',
  status: 'basic',
  domains: 'domains',
  https_enabled: 'https',
  certificate_ids: 'https',
  minimum_tls_version: 'https',
  force_https: 'https',
  hsts_enabled: 'https',
  origins: 'origins',
  default_origin_group: 'origins',
  origin_host_header: 'origin-settings',
  origin_http_version: 'origin-settings',
  origin_connect_timeout_seconds: 'origin-settings',
  origin_read_timeout_seconds: 'origin-settings',
  pass_client_ip: 'origin-settings',
  health_check_enabled: 'health',
  health_check_path: 'health',
  health_check_interval_seconds: 'health',
  health_check_timeout_seconds: 'health',
  health_check_expected_status: 'health',
  healthy_threshold: 'health',
  unhealthy_threshold: 'health',
  route_rules: 'routes',
  response_compression_enabled: 'compression',
  response_compression_min_bytes: 'compression',
  response_compression_max_bytes: 'compression',
  response_compression_algorithms: 'compression',
  response_compression_mime_types: 'compression',
  response_compression_extensions: 'compression',
  response_compression_excluded_extensions: 'compression',
  access_log_enabled: 'logs',
  access_log_request_headers: 'logs',
  access_log_request_body: 'logs',
  access_log_response_headers: 'logs',
  access_log_query_params: 'logs',
  access_log_cookies: 'logs',
  access_log_referer: 'logs',
  access_log_user_agent: 'logs',
  access_log_status_code_ranges: 'logs',
  access_log_client_abort: 'logs',
}

/** Group owning an error path such as `origins.0.host`. */
export function websiteSectionForPath(path: string): WebsiteSectionId {
  const field = path.split('.')[0] as keyof WebsiteFormValues
  return fieldSections[field] ?? defaultWebsiteSection
}

export type WebsiteFormErrors = Record<string, string>

export type WebsiteSectionProps = {
  values: WebsiteFormValues
  errors: WebsiteFormErrors
  update: <K extends keyof WebsiteFormValues>(
    key: K,
    value: WebsiteFormValues[K]
  ) => void
  replace: (change: (current: WebsiteFormValues) => WebsiteFormValues) => void
  /** Saved website in edit mode; absent while creating. */
  website?: Website
}

/** First message per dotted path, as rendered next to each field. */
export function collectWebsiteFormErrors(
  issues: readonly { path: readonly PropertyKey[]; message: string }[]
): WebsiteFormErrors {
  const errors: WebsiteFormErrors = {}
  for (const issue of issues)
    errors[issue.path.map(String).join('.')] ??= issue.message
  return errors
}

/** Error count per group, for the navigation badges. */
export function countWebsiteSectionErrors(errors: WebsiteFormErrors) {
  const counts: Partial<Record<WebsiteSectionId, number>> = {}
  for (const path of Object.keys(errors)) {
    const section = websiteSectionForPath(path)
    counts[section] = (counts[section] ?? 0) + 1
  }
  return counts
}

/** Where to take the user after a failed validation: the earliest group in
 * navigation order with an error and, for route rules, the earliest rule. */
export function firstWebsiteErrorTarget(errors: WebsiteFormErrors):
  | {
      section: WebsiteSectionId
      routeRuleIndex?: number
    }
  | undefined {
  const paths = Object.keys(errors)
  const section = websiteSectionIds.find((id) =>
    paths.some((path) => websiteSectionForPath(path) === id)
  )
  if (!section) return undefined
  if (section !== 'routes') return { section }
  const indexes = paths
    .map((path) => /^route_rules\.(\d+)\./.exec(path)?.[1])
    .filter((index): index is string => index !== undefined)
    .map(Number)
  return indexes.length
    ? { section, routeRuleIndex: Math.min(...indexes) }
    : { section }
}

/** Errors of one route rule keyed by the field path inside the rule. */
export function routeRuleErrors(
  errors: WebsiteFormErrors,
  index: number
): WebsiteFormErrors {
  const prefix = `route_rules.${index}.`
  return Object.fromEntries(
    Object.entries(errors)
      .filter(([path]) => path.startsWith(prefix))
      .map(([path, message]) => [path.slice(prefix.length), message])
  )
}
