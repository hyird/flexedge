import { useQuery } from '@tanstack/react-query'
import { getData, type PageData } from '@/lib/api'
import { queryKeys } from '@/lib/query-keys'

export const taskTypes = {
  provider: '服务商检测',
  dns_zone: 'DNS 同步',
  certificate: '证书签发',
  website: '网站域名检查',
  node: '节点发布',
} as const

export const taskStatuses = {
  queued: '排队中',
  running: '执行中',
  retrying: '重试中',
  completed: '成功',
  recovered: '已恢复',
  failed: '失败',
  superseded: '已结束',
} as const

export type Task = {
  id: string
  resource_type: keyof typeof taskTypes
  resource_id: string
  name: string
  operation: string
  version: number
  status: keyof typeof taskStatuses
  error: string
  failures: number
  updated_at: string
  next_attempt_at: string
}

export type TaskPage = PageData<Task> & { active: number; failed: number }
export type TaskHistory = {
  list: { outcome: 'completed' | 'failed'; error: string; emitted_at: string }[]
  truncated: boolean
}

export function taskKey(task: Task) {
  return `${task.resource_type}:${task.id}:${task.resource_id}:${task.version}`
}

export function taskTitle(task: Task) {
  if (task.resource_type === 'certificate' && task.operation === 'renew')
    return '证书续期'
  if (task.resource_type === 'dns_zone' && task.operation === 'delete')
    return 'DNS 清理'
  return taskTypes[task.resource_type]
}

export const resourceLinks = {
  provider: '/providers',
  dns_zone: '/dns-zones',
  certificate: '/certificates',
  website: '/websites',
  node: '/nodes',
} as const

export function useTasks(
  params: {
    page?: number
    page_size?: number
    type?: string
    status?: string
    keyword?: string
    days?: number
  } = {}
) {
  return useQuery({
    queryKey: [...queryKeys.tasks, 'list', params],
    queryFn: () => getData<TaskPage>('/tasks', params),
  })
}
