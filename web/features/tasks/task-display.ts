import {
  defaultStringifySearch,
  type HistoryState,
} from '@tanstack/react-router'
import { parseDate } from '@/lib/format'
import { statusReserve, type StatusDictionary } from '@/lib/status'
import {
  known,
  taskResourceTypes,
  type Task,
  type TaskRef,
  type taskStatusValues,
} from './types'

/** Task name per resource type (one task per resource). */
export const taskTypeLabels = {
  cache: '缓存操作',
  website: '网站检查',
  dns_zone: 'DNS同步',
  certificate: '证书签发',
  provider: '服务商验证',
  node: '节点配置下发',
} as const satisfies Record<(typeof taskResourceTypes)[number], string>

/** Operation the task currently runs (and the operation of each attempt). */
export const taskOperations = {
  prewarm: '预热缓存',
  refresh: '刷新缓存',
  clean: '清理缓存',
  apply: '检查网站',
  sync: '同步',
  delete: '删除托管域',
  issue: '签发',
  renew: '续期',
  verify: '验证',
  publish: '发布配置',
  configure: '下发节点配置',
} as const

/** Task statuses with their chip colour. */
export const taskStatuses = {
  queued: { label: '排队中', tone: 'default' },
  running: { label: '执行中', tone: 'accent' },
  retrying: { label: '重试中', tone: 'warning' },
  failed: { label: '失败', tone: 'danger' },
  completed: { label: '成功', tone: 'success' },
} as const satisfies StatusDictionary<(typeof taskStatusValues)[number]>
export const taskStatusReserve = statusReserve(taskStatuses)

/** A resource's sync-task state as its read model derives it from the task
 * (certificate distribution `sync_status` in certificate_read_service.h):
 * the task statuses under the values that read model sends. */
export const syncTaskStatuses = {
  pending: taskStatuses.queued,
  running: taskStatuses.running,
  retry: taskStatuses.retrying,
  completed: taskStatuses.completed,
} as const satisfies StatusDictionary<
  'pending' | 'running' | 'retry' | 'completed'
>
export const syncTaskStatusReserve = statusReserve(syncTaskStatuses)

/** Status groups of the list filter. */
export const taskStatusGroups = {
  active: '进行中',
  attention: '需关注',
} as const

export const activeTaskStatuses: readonly string[] = [
  'queued',
  'running',
  'retrying',
]

/** Recent-task order matches the paginated task list: newest execution first. */
export function recentTaskOrder<T extends Pick<Task, 'updated_at'>>(
  list: readonly T[]
): T[] {
  return [...list].sort(
    (left, right) =>
      (parseDate(right.updated_at)?.getTime() ?? 0) -
      (parseDate(left.updated_at)?.getTime() ?? 0)
  )
}

export function operationLabel(operation: string) {
  return known(taskOperations, operation) ?? operation
}

/** `证书续期`, `删除托管域`: the type name, specialised by the operation
 * where it names a different job. */
export function taskTypeLabel(task: Pick<Task, 'resource_type' | 'operation'>) {
  if (task.resource_type === 'cache')
    return (
      (
        {
          prewarm: '缓存预热',
          refresh: '刷新缓存',
          clean: '清理缓存',
        } as Record<string, string>
      )[task.operation] ?? '缓存操作'
    )
  if (task.resource_type === 'certificate' && task.operation === 'renew')
    return '证书续期'
  if (task.resource_type === 'dns_zone' && task.operation === 'delete')
    return '删除托管域'
  return known(taskTypeLabels, task.resource_type) ?? task.resource_type
}

/** `DNS 同步 · a-z.xin`. */
export function taskTitle(
  task: Pick<Task, 'resource_type' | 'operation' | 'name'>
) {
  return `${taskTypeLabel(task)} · ${task.name}`
}

/** `/tasks?task=` value: `<resource_type>:<resource_id>`. */
export function taskKey(task: TaskRef) {
  return `${task.resourceType}:${task.resourceId}`
}

/** Inverse of `taskKey`; `undefined` for anything else. */
export function parseTaskKey(value: string): TaskRef | undefined {
  const separator = value.indexOf(':')
  const resourceType = value.slice(0, separator)
  const resourceId = value.slice(separator + 1)
  if (
    separator < 0 ||
    !(taskResourceTypes as readonly string[]).includes(resourceType) ||
    !/^[\w-]+$/.test(resourceId)
  )
    return undefined
  return { resourceType, resourceId }
}

/** Deep link that opens a task's detail sheet. */
export function taskHref(task: TaskRef) {
  return `/tasks?${new URLSearchParams({ task: taskKey(task) })}`
}

/** Page of the task's resource; `undefined` for types without one. A deleted
 * resource has no task, so the page always exists. Providers have no detail
 * page: the link opens the provider list of `resource_kind` searched for the
 * account. */
export function taskTarget(
  task: Pick<Task, 'resource_type' | 'resource_id' | 'resource_kind' | 'name'>
): { href: string; state?: HistoryState } | undefined {
  const encoded = encodeURIComponent(task.resource_id)
  switch (task.resource_type) {
    case 'cache':
      return { href: '/cache' }
    case 'website':
      return { href: '/websites', state: { website: { id: task.resource_id } } }
    case 'node':
      return { href: `/nodes/${encoded}` }
    case 'dns_zone':
      return { href: `/dns-zones/${encoded}` }
    case 'certificate':
      return { href: `/certificates/${encoded}` }
    case 'provider': {
      const list = task.resource_kind === 'certificate' ? 'certificate' : 'dns'
      return {
        href: `/providers${defaultStringifySearch({ q: task.name })}`,
        state: { providers: { tab: list } },
      }
    }
    default:
      return undefined
  }
}

/** What a task shows in its fixed note slot next to the status, in priority
 * order; `null` when nothing applies. */
export type TaskNote = { kind: 'offline' } | { kind: 'retry'; at: string }

export function taskNote(
  task: Pick<Task, 'status' | 'node_offline' | 'next_attempt_at'>
): TaskNote | null {
  // The backend sets `node_offline` only while the task is unfinished.
  if (task.node_offline) return { kind: 'offline' }
  if (task.status === 'retrying' && task.next_attempt_at)
    return { kind: 'retry', at: task.next_attempt_at }
  return null
}
