import { formatRelative } from '@/lib/format'
import { taskAttemptEventSchema, type TaskAttempt } from './types'

/** Execution records kept on screen (the `history` event carries 50). */
export const historyRecordLimit = 100

/** Merges an `attempt` event into the task's records: the new record goes on
 * top, an older copy of the same attempt (`id`, e.g. replayed after a
 * reconnect) is dropped, the summary is replaced and the list keeps the
 * newest `historyRecordLimit`. Works on wire frames; returns `undefined` (no
 * change) for a malformed event or before the first `history` frame. */
export function mergeAttemptEvent<
  H extends { list: readonly { id: string }[]; summary: unknown },
>(history: H | undefined, value: unknown): H | undefined {
  const event = taskAttemptEventSchema.safeParse(value)
  if (!history || !event.success) return undefined
  const { item, summary } = event.data
  return {
    ...history,
    list: [
      item,
      ...history.list.filter((record) => record.id !== item.id),
    ].slice(0, historyRecordLimit),
    summary,
  } as H
}

/** One history entry: a single attempt, or a run of consecutive failed
 * attempts with the same error (`attempts.length > 1`, newest first). The key
 * names the oldest attempt, so a new failure joining the run keeps it. */
export type AttemptGroup = {
  key: string
  attempts: TaskAttempt[]
}

/** Collapses consecutive failures of the same operation with the same error
 * into one entry; a success between two failures always breaks the run. */
export function groupAttempts(list: readonly TaskAttempt[]): AttemptGroup[] {
  const groups: AttemptGroup[] = []
  for (const attempt of list) {
    const group = groups.at(-1)
    const run = group?.attempts[0]
    const key = attempt.id
    const repeatsFailure =
      run?.outcome === 'failed' &&
      attempt.outcome === 'failed' &&
      attempt.operation === run.operation &&
      attempt.error !== '' &&
      attempt.error === run.error
    if (group && repeatsFailure) {
      group.attempts.push(attempt)
      group.key = key
    } else groups.push({ key, attempts: [attempt] })
  }
  return groups
}

/** Duration: `878 毫秒`, `12.4 秒`, `2 分 5 秒`, `1 小时 3 分`. */
export function formatDuration(ms: number) {
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

/** Live elapsed time of unsettled work in whole seconds (`0 秒`, `42 秒`,
 * `2 分 5 秒`), so the label ticks once per second. */
export function formatElapsed(ms: number) {
  const seconds = Math.max(0, Math.floor(ms / 1000))
  return seconds < 60 ? `${seconds} 秒` : formatDuration(seconds * 1000)
}

/** Attempt duration; records without a start time are unknown. */
export function formatAttemptDuration(attempt: {
  started_at: string
  duration_ms: number
}) {
  if (!attempt.started_at) return '—'
  return formatDuration(attempt.duration_ms)
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
