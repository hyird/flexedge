import type { z } from 'zod'
import { queryOptions } from '@tanstack/react-query'
import { streamPath } from '@/lib/api'
import { ApiProtocolError } from '@/lib/api-response'
import {
  readLiveEvent,
  readLiveQuery,
  readLiveSnapshot,
  snapshotQueryOptions,
} from '@/lib/live-query'
import { validateCollectionPage } from '@/lib/pagination'
import { queryKeys } from '@/lib/query-keys'
import { mergeAttemptEvent } from './task-history'
import {
  operationDetailSchema,
  operationPageSchema,
  subtaskHistorySchema,
  taskAttemptTraceSchema,
} from './types'

/** List filters of `/api/operations/stream` (contract §2.1). */
export type OperationFilters = {
  page?: number
  page_size?: number
  status?: string
  action?: string
  target_type?: string
  actor?: string
  days?: number
  keyword?: string
}

function parse<T>(schema: z.ZodType<T>, value: unknown): T {
  const result = schema.safeParse(value)
  if (!result.success) throw new ApiProtocolError()
  return result.data
}

function parsePage(value: unknown, page: number) {
  const data = parse(operationPageSchema, value)
  validateCollectionPage(data, page)
  return data
}

/** Task list on `/tasks`: live while observed (the page disables it while
 * the detail sheet owns the page's stream). */
export function operationsQuery(params: OperationFilters = {}) {
  return queryOptions({
    retry: false,
    queryKey: [...queryKeys.operations, 'list', params],
    queryFn: ({ queryKey, signal }) =>
      readLiveQuery(
        streamPath('/operations', params),
        queryKey,
        signal,
        (data) => parsePage(data, params.page ?? 1)
      ),
  })
}

/** One-shot read of the newest tasks (first frame, then the stream closes):
 * the header task center reads 20, the overview 6. The `once` key keeps it
 * apart from the live list. */
export function recentOperationsQuery(pageSize: number) {
  return snapshotQueryOptions(
    streamPath('/operations', { page: 1, page_size: pageSize }),
    [...queryKeys.operations, 'once', pageSize],
    (data) => parsePage(data, 1)
  )
}

/** Task detail: the open sheet's only live stream. `subtaskId` is the
 * expanded subtask; changing it changes the URL, so the old connection is
 * released before the new one opens. The task's previous snapshot stays on
 * screen meanwhile (no skeleton for the header or subtask list). */
export function operationDetailQuery(id: string, subtaskId: string | null) {
  return queryOptions({
    retry: false,
    queryKey: [...queryKeys.operations, 'detail', id, subtaskId ?? ''],
    queryFn: ({ queryKey, signal }) =>
      readLiveQuery(
        operationStreamPath(id, subtaskId),
        queryKey,
        signal,
        (data) => parse(operationDetailSchema, data)
      ),
    placeholderData: (previous, previousQuery) =>
      previousQuery?.queryKey[2] === id ? previous : undefined,
  })
}

function operationStreamPath(id: string, subtaskId: string | null) {
  return streamPath(`/operations/${encodeURIComponent(id)}`, {
    subtask_id: subtaskId ?? undefined,
  })
}

/** Execution records of the expanded subtask: the `history` event of the
 * detail stream (same connection), with each `attempt` event merged in
 * (contract §3.2). A new `history` frame replaces the records. */
export function subtaskHistoryQuery(id: string, subtaskId: string) {
  return queryOptions({
    retry: false,
    queryKey: [...queryKeys.operations, 'detail', id, subtaskId, 'records'],
    queryFn: ({ queryKey, signal }) =>
      readLiveEvent(
        operationStreamPath(id, subtaskId),
        'history',
        queryKey,
        signal,
        (data) => parse(subtaskHistorySchema, data),
        (current, value, event) =>
          event === 'attempt' ? mergeAttemptEvent(current, value) : undefined
      ),
  })
}

/** Trace reads open at once; later reads wait in request order. */
export const traceReadLimit = 2
let openTraceReads = 0
const queuedTraceReads = new Set<() => void>()

/** Takes one of the `traceReadLimit` slots, waiting while all are taken. A
 * read cancelled while waiting leaves the queue without opening a stream. */
function acquireTraceRead(signal: AbortSignal) {
  if (signal.aborted) return Promise.reject(signal.reason)
  if (openTraceReads < traceReadLimit) {
    openTraceReads += 1
    return Promise.resolve()
  }
  return new Promise<void>((resolve, reject) => {
    const start = () => {
      queuedTraceReads.delete(start)
      signal.removeEventListener('abort', cancel)
      openTraceReads += 1
      resolve()
    }
    const cancel = () => {
      queuedTraceReads.delete(start)
      reject(signal.reason)
    }
    queuedTraceReads.add(start)
    signal.addEventListener('abort', cancel, { once: true })
  })
}

function releaseTraceRead() {
  openTraceReads -= 1
  queuedTraceReads.values().next().value?.()
}

/** Full trace of one attempt: a one-shot snapshot (the stream closes after
 * its first frame), never part of the records themselves. It is read only
 * once the attempt is `visible`, with at most `traceReadLimit` reads open at
 * once. Stored traces never change, so the snapshot is cached for good: no
 * event reads it again. Closing the sheet cancels pending and queued reads. */
export function taskAttemptTraceQuery(
  operationId: string,
  attemptId: string,
  visible = true
) {
  return queryOptions({
    retry: false,
    staleTime: Infinity,
    gcTime: Infinity,
    enabled: visible,
    queryKey: [...queryKeys.operations, 'trace', operationId, attemptId],
    queryFn: async ({ queryKey, signal }) => {
      await acquireTraceRead(signal)
      try {
        return await readLiveSnapshot(
          streamPath(
            `/operations/${encodeURIComponent(operationId)}/attempts/${attemptId}/trace`
          ),
          queryKey,
          signal,
          (data) => parse(taskAttemptTraceSchema, data)
        )
      } finally {
        releaseTraceRead()
      }
    },
  })
}
