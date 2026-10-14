import type { NavigationTarget } from '@/lib/history-state'
import type { StatusTone } from '@/lib/status'
import type { Task } from './types'

export const taskTypes = {
  provider: '服务商检测',
  dns_zone: 'DNS 同步',
  certificate: '证书签发',
  website: '网站域名检查',
  node: '节点发布',
} as const satisfies Record<Task['resource_type'], string>

export const taskStatuses = {
  queued: '排队中',
  running: '执行中',
  retrying: '重试中',
  completed: '成功',
  recovered: '已恢复',
  failed: '失败',
  superseded: '已结束',
} as const satisfies Record<Task['status'], string>

/** Every task status label, for `StatusChip`'s `reserve`. */
export const taskStatusLabels = Object.values(taskStatuses)

export const taskStatusTones = {
  queued: 'accent',
  running: 'accent',
  retrying: 'warning',
  completed: 'success',
  recovered: 'success',
  failed: 'danger',
  superseded: 'default',
} as const satisfies Record<Task['status'], StatusTone>

export const activeTaskStatuses: readonly Task['status'][] = [
  'queued',
  'running',
  'retrying',
]

export function taskKey(task: Task) {
  return `${task.resource_type}:${task.id}:${task.resource_id}:${task.version}`
}

export function taskTitle(task: {
  resource_type: Task['resource_type'] | (string & {})
  operation: string
}) {
  if (task.resource_type === 'certificate' && task.operation === 'renew')
    return '证书续期'
  if (task.resource_type === 'dns_zone' && task.operation === 'delete')
    return 'DNS 清理'
  return (
    taskTypes[task.resource_type as Task['resource_type']] ?? task.resource_type
  )
}

/** Deep link that opens the task detail sheet. Without `version` the page
 * resolves the latest version of the task for that resource. */
export function taskHref(task: {
  id: string
  resource_id: string
  version?: number
}) {
  const search = new URLSearchParams({
    task: task.id,
    resource_id: task.resource_id,
  })
  if (task.version !== undefined) search.set('version', String(task.version))
  return `/tasks?${search}`
}

// Certificate providers have no account name, so the task row carries the
// provider code instead (see service/domains/task/task_service.h).
const certificateProviderCodes: Record<string, true> = {
  letsencrypt: true,
  zerossl: true,
}

/** Detail page of the task's resource. Providers have no detail page, so the
 * provider list opens on the matching tab filtered to the account; the tab and
 * keyword travel as history state, never in the URL. */
export function taskResourceTarget(task: {
  resource_type: Task['resource_type'] | (string & {})
  resource_id: string
  name: string
}): NavigationTarget {
  const id = encodeURIComponent(task.resource_id)
  switch (task.resource_type) {
    case 'dns_zone':
      return { href: `/dns-zones/${id}` }
    case 'certificate':
      return { href: `/certificates/${id}` }
    case 'website':
      return { href: `/websites/${id}` }
    case 'node':
      return { href: `/nodes/${id}` }
    case 'provider':
      return {
        href: '/providers',
        state: {
          providers: {
            tab: certificateProviderCodes[task.name] ? 'certificate' : 'dns',
            q: task.name,
          },
        },
      }
    default:
      return { href: '/tasks' }
  }
}
