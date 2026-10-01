import type { CacheJob } from './cache-job-types'
import type { Website } from './types'

export type CacheJobInput = Pick<
  CacheJob,
  'operation' | 'mode' | 'targets' | 'concurrency' | 'rate_limit_bps'
>

export type CacheJobFormValues = {
  operation: CacheJobInput['operation']
  mode: CacheJobInput['mode']
  targets: string
  concurrency: string
  rate_mib: string
}

type CacheJobFormResult =
  { ok: true; input: CacheJobInput } | { ok: false; error: string }

export function parseCacheJobInput(
  website: Pick<Website, 'config'>,
  values: CacheJobFormValues
): CacheJobFormResult {
  const mode = values.operation === 'preheat' ? 'url' : values.mode
  const targets =
    mode === 'all'
      ? []
      : values.targets
          .split(/\r?\n/)
          .map((target) => target.trim())
          .filter(Boolean)
  if (mode !== 'all') {
    if (
      !targets.length ||
      targets.length > 100 ||
      new Set(targets).size !== targets.length
    )
      return { ok: false, error: '请输入 1–100 个不重复的 URL。' }
    if (mode === 'prefix' && targets.length !== 1)
      return { ok: false, error: '目录刷新只能填写一个前缀 URL。' }
    for (const target of targets) {
      try {
        const url = new URL(target)
        if (
          !['http:', 'https:'].includes(url.protocol) ||
          url.username ||
          url.password ||
          url.hash ||
          url.port ||
          /[\\\s]/.test(target)
        )
          return {
            ok: false,
            error:
              'URL 必须使用 HTTP/HTTPS，不含用户信息、非默认端口、空白或片段。',
          }
        if (
          !website.config.domains.some(
            (domain) =>
              domain.hostname.toLowerCase() === url.hostname.toLowerCase()
          )
        )
          return { ok: false, error: `域名 ${url.hostname} 未绑定到此网站。` }
        if (url.protocol === 'https:' && !website.config.https_enabled)
          return { ok: false, error: '此网站尚未启用 HTTPS，请使用 HTTP URL。' }
      } catch {
        return { ok: false, error: '请输入有效的 HTTP/HTTPS URL。' }
      }
    }
  }
  // Purge does not expose or use preheat scheduling controls.
  const concurrency =
    values.operation === 'preheat' ? Number(values.concurrency) : 2
  const rate = values.operation === 'preheat' ? Number(values.rate_mib) : 5
  if (
    !Number.isInteger(concurrency) ||
    concurrency < 1 ||
    concurrency > 4 ||
    !Number.isInteger(rate) ||
    rate < 1 ||
    rate > 100
  )
    return {
      ok: false,
      error: '并发数必须是 1–4 的整数，速率必须是 1–100 MiB/s 的整数。',
    }
  return {
    ok: true,
    input: {
      operation: values.operation,
      mode,
      targets,
      concurrency,
      rate_limit_bps: rate * 1048576,
    },
  }
}
