import { formatRelative } from '@/lib/format'
import type { TaskAttempt } from './types'

/** One history entry: a single attempt, or a run of consecutive failed
 * attempts with the same error (`attempts.length > 1`, newest first). */
export type AttemptGroup = {
  key: string
  attempts: TaskAttempt[]
}

/** Collapses consecutive failures with the same error into one entry; a
 * success between two failures always breaks the run. */
export function groupAttempts(list: readonly TaskAttempt[]): AttemptGroup[] {
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
  return groups
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
