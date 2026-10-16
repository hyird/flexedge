import {
  queryOptions,
  type QueryKey,
  type UseQueryOptions,
} from '@tanstack/react-query'
import { readLiveAppend } from '@/lib/live-query'
import { mergeLiveLogs, type LogEntry } from './log-event'

export type LiveLogsQuery<T extends LogEntry> = UseQueryOptions<
  T[],
  Error,
  T[],
  QueryKey
>

/** Live tail of `url`: each `logs` frame is appended (deduplicated by id,
 * newest first, at most `liveLogCapacity`). After an interruption the
 * stream resumes from its last cursor (`Last-Event-ID`). The records are
 * dropped once the tail's view closes. */
export function liveLogsQuery<T extends LogEntry>(
  url: string,
  queryKey: QueryKey,
  parse: (value: unknown) => T[]
): LiveLogsQuery<T> {
  // Generic factory: callers own the key, which must identify `url`.
  // eslint-disable-next-line @tanstack/query/exhaustive-deps
  return queryOptions({
    queryKey,
    retry: false,
    gcTime: 0,
    meta: { inlineError: true },
    queryFn: ({ signal }) =>
      readLiveAppend(url, 'logs', queryKey, signal, parse, mergeLiveLogs),
  })
}
