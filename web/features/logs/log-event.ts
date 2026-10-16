import { z } from 'zod'
import { ApiProtocolError } from '@/lib/api-response'

export type LogEntry = { id: string; occurred_at: string }
/** Records a live tail keeps, newest first. */
export const liveLogCapacity = 10_000

/** Parses the records of one `logs` frame (its envelope data). */
export function createLogEventParser<T>(entry: z.ZodType<T>) {
  const data = z.object({ list: z.array(entry) })
  return (value: unknown): T[] => {
    const result = data.safeParse(value)
    if (!result.success) throw new ApiProtocolError()
    return result.data.list
  }
}

export function mergeLiveLogs<T extends LogEntry>(
  current: readonly T[] | undefined,
  incoming: readonly T[]
): T[] {
  return [
    ...new Map(
      [...(current ?? []), ...incoming].map((entry) => [entry.id, entry])
    ).values(),
  ]
    .sort((left, right) => right.occurred_at.localeCompare(left.occurred_at))
    .slice(0, liveLogCapacity)
}
