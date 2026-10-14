import { formatRelative } from '@/lib/format'
import type { TaskAttempt } from './types'

/** One history entry: a single attempt, or a run of consecutive failed
 * attempts with the same error (`attempts.length > 1`, newest first). */
export type AttemptGroup = {
  key: string
  attempts: TaskAttempt[]
}

/** Collapses consecutive failures with the same error into one entry. Runs
 * are found on the full list before filtering, so a success between two
 * failures always breaks the run even when only failures are shown. */
export function groupAttempts(
  list: readonly TaskAttempt[],
  { failuresOnly = false }: { failuresOnly?: boolean } = {}
): AttemptGroup[] {
  const groups: AttemptGroup[] = []
  for (const attempt of list) {
    const run = groups.at(-1)?.attempts[0]
    const repeatsFailure =
      run?.outcome === 'failed' &&
      attempt.outcome === 'failed' &&
      attempt.error !== '' &&
      attempt.error === run.error
    if (repeatsFailure) groups.at(-1)?.attempts.push(attempt)
    else
      groups.push({
        key: `${attempt.attempt}:${attempt.emitted_at}`,
        attempts: [attempt],
      })
  }
  return failuresOnly
    ? groups.filter((group) => group.attempts[0].outcome === 'failed')
    : groups
}

/** Attempt duration: `878 毫秒`, `12.4 秒`, `2 分 5 秒`, `1 小时 3 分`.
 * Records without a start time (written before the upgrade) are unknown. */
export function formatAttemptDuration(attempt: {
  started_at: string
  duration_ms: number
}) {
  if (!attempt.started_at) return '—'
  const ms = attempt.duration_ms
  if (ms < 1000) return `${ms} 毫秒`
  if (ms < 60_000) return `${Math.floor(ms / 100) / 10} 秒`
  const totalSeconds = Math.floor(ms / 1000)
  if (totalSeconds < 3600) {
    const seconds = totalSeconds % 60
    return `${Math.floor(totalSeconds / 60)} 分${seconds ? ` ${seconds} 秒` : ''}`
  }
  const minutes = Math.floor(totalSeconds / 60) % 60
  return `${Math.floor(totalSeconds / 3600)} 小时${minutes ? ` ${minutes} 分` : ''}`
}

/** Label of a scheduled retry against the local clock: whole seconds while
 * under a minute away, then the shared relative format. Once due it reads
 * `已到重试时间`; the snapshot cannot know whether the retry already ran. */
export function formatRetryCountdown(at: number, now: number) {
  const seconds = Math.ceil((at - now) / 1000)
  if (seconds <= 0) return '已到重试时间'
  if (seconds < 60) return `${seconds} 秒后`
  return formatRelative(new Date(at).toISOString(), now)
}

/** Certificate issuance stages in execution order: `[stage, short label,
 * description]`. Successful attempts report `completed`. When every
 * authorization is already valid the worker skips `dns_challenge` through
 * `cleanup`; a failed validation still reports `validation`. */
export const certificateStages = [
  ['prepare', '准备', '读取证书、供应商与托管域配置'],
  ['account', '账户', '注册或复用 ACME 账户'],
  ['order', '订单', '创建证书订单'],
  ['authorization', '授权', '获取域名授权'],
  ['dns_challenge', '验证记录', '写入 DNS 验证记录并等待生效'],
  ['validation', 'CA 验证', '证书颁发机构验证域名'],
  ['cleanup', '清理', '清理 DNS 验证记录'],
  ['finalize', '签发', '提交证书请求并等待签发'],
  ['download', '下载', '下载并校验证书'],
  ['persist', '保存', '保存证书'],
] as const

/** Stepper position of a certificate attempt: `failedAt` is the index of the
 * stage it stopped in, or -1 when it completed or the stage is unknown. */
export function certificateProgress(stage: string) {
  if (stage === 'completed')
    return { current: certificateStages.length, failedAt: -1 }
  const failedAt = certificateStages.findIndex(([id]) => id === stage)
  return { current: Math.max(failedAt, 0), failedAt }
}
