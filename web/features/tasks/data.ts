import type { z } from 'zod'
import { queryOptions, skipToken, useQuery } from '@tanstack/react-query'
import { streamPath } from '@/lib/api'
import { readLiveQuery } from '@/lib/live-query'
import { ApiProtocolError } from '@/lib/api-response'
import { validateCollectionPage } from '@/lib/pagination'
import { queryKeys } from '@/lib/query-keys'
import {
  taskSchema,
  taskPageSchema,
  taskHistorySchema,
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

function parse<T>(schema: z.ZodType<T>, value: unknown): T {
  const result = schema.safeParse(value)
  if (!result.success) throw new ApiProtocolError()
  return result.data
}

export function tasksQuery(params: TaskFilters = {}) {
  return queryOptions({
  retry: false,
    queryKey: [...queryKeys.tasks, 'list', params],
    queryFn: ({ queryKey, signal }) => readLiveQuery(streamPath('/tasks', params), queryKey, signal,
      (data) => {
        const page = parse(taskPageSchema, data)
        validateCollectionPage(page, params.page ?? 1)
        return page
      }),
  })
}

export function useTasks(params: TaskFilters = {}) {
  return useQuery(tasksQuery(params))
}

export function taskDetailQuery(task: Task | null) {
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
          readLiveQuery(streamPath(`/tasks/${task.id}`, {
              resource_id: task.resource_id,
              version: task.version,
            }), queryKey, signal, (data) => parse(taskSchema, data))
      : skipToken,
  })
}

export function taskHistoryQuery(task: Task | null) {
  return queryOptions({
  retry: false,
    queryKey: [...queryKeys.tasks, 'history', task?.id, task?.version],
    queryFn:
      task && task.resource_type !== 'node'
        ? ({ queryKey, signal }) =>
            readLiveQuery(streamPath(`/tasks/${task.id}/history`, { version: task.version }),
              queryKey, signal, (data) => parse(taskHistorySchema, data))
        : skipToken,
  })
}
