import { formatDate } from '@/lib/format'
import type { StatusTone } from '@/lib/status'
import type { TraceCall, TraceStep } from './types'

/** Fixed placeholder the backend writes for every hidden value; shown as an
 * "已隐藏" badge, never as raw text. */
export const redactedMarker = '[REDACTED]'

/** `task_error::not_found`: the task or attempt does not exist (or the
 * record expired). */
export const taskNotFoundCode = 16701

/** Splits text around redaction markers: strings are literal text, `null`
 * marks one hidden value. */
export function splitRedacted(text: string): (string | null)[] {
  if (!text.includes(redactedMarker)) return [text]
  return text
    .split(redactedMarker)
    .flatMap((part, index) =>
      index === 0 ? (part ? [part] : []) : part ? [null, part] : [null]
    )
}

export const stepOutcomes = {
  completed: { label: '完成', tone: 'success' },
  failed: { label: '失败', tone: 'danger' },
  skipped: { label: '跳过', tone: 'default' },
  interrupted: { label: '中断', tone: 'warning' },
} as const satisfies Record<string, { label: string; tone: StatusTone }>

export const callOutcomes = {
  ok: '成功',
  http_error: 'HTTP 错误',
  transport_error: '请求未完成',
  cancelled: '已取消',
  invalid_response: '响应无法解析',
} as const

/** Transport error codes of `call.error` / `http.transport_error`. */
export const transportErrors = {
  timeout: '请求超时',
  resolve_failed: '域名解析失败',
  connect_failed: '无法建立连接',
  tls_failed: 'TLS 握手失败',
  response_too_large: '响应过大',
  io_error: '读写错误',
  protocol_error: '协议错误',
  queue_full: '请求队列已满',
  cancelled: '已取消',
  other: '其他错误',
} as const

export const bodyFormats = {
  json: 'JSON',
  jws_decoded: 'JWS 解码',
  form: '表单',
  text: '文本',
  pem_summary: '证书链摘要',
  omitted: '未保存',
} as const

/** Why a body excerpt was not stored. */
export const omittedReasons = {
  secret_material: '含密钥材料未保存',
  binary: '二进制内容未保存',
  too_large: '正文过大未解析',
  budget: '本次正文预算已用尽',
  policy: '按策略不保存',
} as const

export const logLevels = {
  info: { label: '信息', tone: 'default' },
  warning: { label: '警告', tone: 'warning' },
  error: { label: '错误', tone: 'danger' },
} as const satisfies Record<string, { label: string; tone: StatusTone }>

export const workerTypes = {
  'dns-sync': 'DNS 同步 Worker',
  certificate: '证书 Worker',
  'website-dispatch': '网站分发 Worker',
  'provider-verification': '供应商验证 Worker',
  web: '节点控制连接',
} as const

/** Tone of an HTTP status: 2xx success, 4xx/5xx and transport failures
 * (status 0) danger, anything else neutral. */
export function httpStatusTone(status: number): StatusTone {
  if (status >= 200 && status < 300) return 'success'
  if (status === 0 || status >= 400) return 'danger'
  return 'default'
}

/** Compact step/call duration: `12 ms`, `5.01 s`, `12.4 s`, `2 min 5 s`. */
export function formatTraceDuration(ms: number) {
  if (ms < 1000) return `${ms} ms`
  if (ms < 10_000) return `${(Math.floor(ms / 10) / 100).toFixed(2)} s`
  if (ms < 60_000) return `${(Math.floor(ms / 100) / 10).toFixed(1)} s`
  const seconds = Math.floor(ms / 1000)
  return `${Math.floor(seconds / 60)} min${seconds % 60 ? ` ${seconds % 60} s` : ''}`
}

/** Zone of the console time `formatDate` shows (Asia/Shanghai, no DST);
 * labelled once per trace because log-style clocks are often read as UTC. */
export const consoleTimeZoneLabel = 'UTC+8'

/** Console time with milliseconds (`2026-10-14 22:16:49.138`) of a backend
 * time (`…HH:MM:SS.ffffff+00`); '' when unknown. Same zone and layout as
 * `formatDate` elsewhere in the task center; the fraction is cut to
 * milliseconds as written rather than rounded through `Date`. */
export function formatTraceTime(value: string) {
  const match =
    /^(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2})(?:\.(\d+))?(Z|[+-]\d{2}(?::?\d{2})?)?$/.exec(
      value
    )
  if (!match) return ''
  const zone = (match[3] ?? '').replace(/^([+-]\d{2})(\d{2})$/, '$1:$2')
  const seconds = formatDate(`${match[1]}${zone}`)
  if (!/^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}$/.test(seconds)) return ''
  return `${seconds}.${(match[2] ?? '').padEnd(3, '0').slice(0, 3)}`
}

/** Clock part of `formatTraceTime` (`22:16:49.138`); '' when unknown. */
export function formatTraceClock(value: string) {
  return formatTraceTime(value).slice(11)
}

/** Node message fields that identify a config release or revision
 * (`release_id`, and `*revision` / `*digest` / `*generation` should a newer
 * node add them). The console never shows config version identifiers. */
export function isConfigVersionField(name: string) {
  return /(?:^|_)(?:release_id|revision|digest|generation)$/i.test(name)
}

/** `host` + `path` of an HTTP call for its one-line row; falls back to the
 * URL when the parts are missing. Query strings stay in the expanded view. */
export function httpTarget(http: NonNullable<TraceCall['http']>) {
  if (http.host) return `${http.host}${http.path || '/'}`
  return http.url.replace(/^[a-z]+:\/\//i, '').replace(/\?.*$/, '')
}

/** Whether a step has anything to expand. */
export function stepHasDetail(step: TraceStep) {
  return (
    step.calls.length > 0 ||
    step.logs.length > 0 ||
    step.error !== '' ||
    step.related_task !== null ||
    step.omitted_calls > 0
  )
}
