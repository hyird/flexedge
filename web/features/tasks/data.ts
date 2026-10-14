import type { z } from 'zod'
import { queryOptions, skipToken } from '@tanstack/react-query'
import { streamPath } from '@/lib/api'
import { ApiProtocolError } from '@/lib/api-response'
import { readLiveQuery, readLiveSnapshot } from '@/lib/live-query'
import { validateCollectionPage } from '@/lib/pagination'
import { queryKeys } from '@/lib/query-keys'
import {
  taskSchema,
  taskPageSchema,
  taskHistorySchema,
  taskAttemptTraceSchema,
  type Task,
} from './types'

type TaskFilters = {
  page?: number
  page_size?: number
  type?: string
  status?: string
  keyword?: string
  days?: number
}

/** Identity of one task record; the detail stream needs all three. */
type TaskSelection = Pick<Task, 'id' | 'resource_id' | 'version'>

function parse<T>(schema: z.ZodType<T>, value: unknown): T {
  const result = schema.safeParse(value)
  if (!result.success) throw new ApiProtocolError()
  return result.data
}

function parsePage(value: unknown, page: number) {
  const data = parse(taskPageSchema, value)
  validateCollectionPage(data, page)
  return data
}

export function tasksQuery(params: TaskFilters = {}) {
  return queryOptions({
    retry: false,
    queryKey: [...queryKeys.tasks, 'list', params],
    queryFn: ({ queryKey, signal }) =>
      readLiveQuery(streamPath('/tasks', params), queryKey, signal, (data) =>
        parsePage(data, params.page ?? 1)
      ),
  })
}

/** Number of `status` tasks updated in the last 24 hours: a one-shot snapshot
 * of the filtered list total. `trigger` (the live list's update time) re-reads
 * it whenever the task list changes, so no extra subscription stays open. */
export function recentTaskCountQuery(status: Task['status'], trigger: number) {
  const params = { days: 1, page: 1, page_size: 1, status }
  return queryOptions({
    retry: false,
    gcTime: 0,
    queryKey: [...queryKeys.tasks, 'count-24h', params, trigger],
    queryFn: ({ queryKey, signal }) =>
      readLiveSnapshot(
        streamPath('/tasks', params),
        queryKey,
        signal,
        (data) => parsePage(data, 1).total
      ),
  })
}

/** Resolves the newest version of a task when a link carries only the task
 * and resource ids (e.g. overview markers). The list keyword also matches
 * resource ids, so one snapshot of that resource's tasks is enough. */
export function latestTaskVersionQuery(
  selection: Pick<Task, 'id' | 'resource_id'> | null
) {
  return queryOptions({
    retry: false,
    gcTime: 0,
    queryKey: [
      ...queryKeys.tasks,
      'latest-version',
      selection?.id,
      selection?.resource_id,
    ],
    queryFn: selection
      ? ({ queryKey, signal }) =>
          readLiveSnapshot(
            streamPath('/tasks', {
              keyword: selection.resource_id,
              page: 1,
              page_size: 100,
            }),
            queryKey,
            signal,
            (data) =>
              parsePage(data, 1)
                .list.filter(
                  (task) =>
                    task.id === selection.id &&
                    task.resource_id === selection.resource_id
                )
                .reduce<number | null>(
                  (latest, task) => Math.max(latest ?? 0, task.version),
                  null
                )
          )
      : skipToken,
  })
}

export function taskDetailQuery(task: TaskSelection | null) {
  return queryOptions({
    retry: false,
    queryKey: [
      ...queryKeys.tasks,
      'detail',
      task?.id,
      task?.resource_id,
      task?.version,
    ],
    queryFn: task
      ? ({ queryKey, signal }) =>
          readLiveQuery(
            streamPath(`/tasks/${task.id}`, {
              resource_id: task.resource_id,
              version: task.version,
            }),
            queryKey,
            signal,
            (data) => parse(taskSchema, data)
          )
      : skipToken,
  })
}

/** Execution history: a one-shot snapshot (the stream closes after its first
 * frame), read when the history tab opens and on manual refresh. It is the
 * brief read allowed next to the page's single live stream. `resource_id`
 * selects one node of a publish (all its nodes share the task id). */
export function taskHistoryQuery(task: TaskSelection | null) {
  return queryOptions({
    retry: false,
    gcTime: 0,
    queryKey: [
      ...queryKeys.tasks,
      'history',
      task?.id,
      task?.resource_id,
      task?.version,
    ],
    queryFn: task
      ? ({ queryKey, signal }) =>
          readLiveSnapshot(
            streamPath(`/tasks/${task.id}/history`, {
              version: task.version,
              resource_id: task.resource_id,
            }),
            queryKey,
            signal,
            (data) => parse(taskHistorySchema, data)
          )
      : skipToken,
  })
}

/** Full trace of one attempt: a one-shot snapshot (the stream closes after
 * its first frame) read only while the attempt's trace view is expanded,
 * never with the history itself. Stored traces never change, so the cached
 * snapshot stays fresh: re-expanding within `gcTime` reads nothing, and no
 * event or refresh invalidates it. */
export function taskAttemptTraceQuery(taskId: string, attemptId: string) {
  return queryOptions({
    retry: false,
    staleTime: Infinity,
    gcTime: 5 * 60_000,
    queryKey: [...queryKeys.tasks, 'trace', taskId, attemptId],
    queryFn: ({ queryKey, signal }) =>
      readLiveSnapshot(
        streamPath(`/tasks/${taskId}/attempts/${attemptId}/trace`),
        queryKey,
        signal,
        (data) => parse(taskAttemptTraceSchema, data)
      ),
  })
}
