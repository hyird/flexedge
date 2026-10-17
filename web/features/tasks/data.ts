import { queryOptions } from '@tanstack/react-query'
import { getData, streamPath } from '@/lib/api'
import { parseResponseData } from '@/lib/api-response'
import { readLiveEvent, readLiveQuery } from '@/lib/live-query'
import { validateCollectionPage } from '@/lib/pagination'
import { queryKeys } from '@/lib/query-keys'
import { mergeAttemptEvent } from './task-history'
import {
  taskAttemptTraceSchema,
  taskHistorySchema,
  taskPageSchema,
  taskSchema,
  type TaskRef,
} from './types'

/** List filters of `/api/tasks` and its stream. */
export type TaskFilters = {
  page?: number
  page_size?: number
  status?: string
  type?: string
  keyword?: string
}

function parsePage(value: unknown, page: number) {
  const data = parseResponseData(taskPageSchema, value)
  validateCollectionPage(data, page)
  return data
}

/** Task list on `/tasks`: live while observed. An open detail sheet takes
 * over the page's stream; the list recovers when it closes. */
export function tasksQuery(params: TaskFilters = {}) {
  return queryOptions({
    retry: false,
    meta: { inlineError: true },
    queryKey: [...queryKeys.tasks, 'list', params],
    queryFn: ({ queryKey, signal }) =>
      readLiveQuery(streamPath('/tasks', params), queryKey, signal, (data) =>
        parsePage(data, params.page ?? 1)
      ),
  })
}

/** The most recently updated tasks, read over GET: the header task center
 * reads 20, the overview 6. Dropped when the view closes, so opening it again
 * reads the current tasks. */
export function recentTasksQuery(pageSize: number) {
  return queryOptions({
    retry: false,
    gcTime: 0,
    queryKey: [...queryKeys.tasks, 'recent', pageSize],
    queryFn: async ({ signal }) =>
      parsePage(
        await getData<unknown>(
          '/tasks',
          { page: 1, page_size: pageSize },
          signal
        ),
        1
      ),
  })
}

function taskPath(resourceType: string, resourceId: string) {
  return `/tasks/${encodeURIComponent(resourceType)}/${encodeURIComponent(resourceId)}`
}

/** Task detail: the open sheet's live stream (`snapshot` event). */
export function taskDetailQuery({ resourceType, resourceId }: TaskRef) {
  return queryOptions({
    retry: false,
    queryKey: [...queryKeys.tasks, 'detail', resourceType, resourceId],
    queryFn: ({ queryKey, signal }) =>
      readLiveQuery(
        streamPath(taskPath(resourceType, resourceId)),
        queryKey,
        signal,
        (data) => parseResponseData(taskSchema, data)
      ),
  })
}

/** Execution records: the `history` event of the detail stream (same
 * connection), with each `attempt` event merged in. A new `history` frame
 * replaces the records. */
export function taskHistoryQuery({ resourceType, resourceId }: TaskRef) {
  return queryOptions({
    retry: false,
    queryKey: [
      ...queryKeys.tasks,
      'detail',
      resourceType,
      resourceId,
      'records',
    ],
    queryFn: ({ queryKey, signal }) =>
      readLiveEvent(
        streamPath(taskPath(resourceType, resourceId)),
        'history',
        queryKey,
        signal,
        (data) => parseResponseData(taskHistorySchema, data),
        (current, value, event) =>
          event === 'attempt' ? mergeAttemptEvent(current, value) : undefined
      ),
  })
}

/** Full trace of one attempt, read over GET once the attempt is `visible`;
 * never part of the records themselves. Stored traces never change, so a
 * trace is cached for good. Closing the sheet cancels pending reads. */
export function taskAttemptTraceQuery(
  { resourceType, resourceId }: TaskRef,
  attemptId: string,
  visible = true
) {
  return queryOptions({
    retry: false,
    staleTime: Infinity,
    gcTime: Infinity,
    enabled: visible,
    queryKey: [
      ...queryKeys.tasks,
      'trace',
      resourceType,
      resourceId,
      attemptId,
    ],
    queryFn: async ({ signal }) =>
      parseResponseData(
        taskAttemptTraceSchema,
        await getData<unknown>(
          `${taskPath(resourceType, resourceId)}/attempts/${encodeURIComponent(attemptId)}/trace`,
          undefined,
          signal
        )
      ),
  })
}
