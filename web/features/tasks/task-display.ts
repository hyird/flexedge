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
