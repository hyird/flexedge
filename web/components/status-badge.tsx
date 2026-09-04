import { Circle, CircleAlert, CircleCheck, LoaderCircle } from 'lucide-react'
import { cn } from '@/lib/utils'
import { Badge } from '@/components/ui/badge'

const labels: Record<string, string> = {
  enabled: '已启用',
  disabled: '已停用',
  online: '在线',
  offline: '离线',
  connected: '已连接',
  disconnected: '未连接',
  registered: '已注册',
  unregistered: '未注册',
  verified: '已验证',
  unverified: '未验证',
  invalid: '无效',
  pending: '等待中',
  running: '执行中',
  retry: '重试中',
  completed: '已完成',
  valid: '有效',
  issuing: '签发中',
  renewing: '续签中',
  failed: '失败',
  expired: '已过期',
  synced: '已同步',
  syncing: '同步中',
  conflict: '有冲突',
  healthy: '健康',
  unhealthy: '异常',
}

const positive = new Set([
  'enabled',
  'online',
  'connected',
  'registered',
  'verified',
  'completed',
  'valid',
  'synced',
  'healthy',
])
const pending = new Set([
  'pending',
  'running',
  'retry',
  'issuing',
  'renewing',
  'syncing',
])
const danger = new Set([
  'failed',
  'expired',
  'invalid',
  'conflict',
  'offline',
  'disconnected',
  'unhealthy',
])

export function StatusBadge({ status }: { status?: string }) {
  const value = status || 'unknown'
  const Icon = positive.has(value)
    ? CircleCheck
    : pending.has(value)
      ? LoaderCircle
      : danger.has(value)
        ? CircleAlert
        : Circle

  return (
    <Badge
      variant='outline'
      className={cn(
        'gap-1.5 font-normal',
        positive.has(value) &&
          'border-emerald-500/30 bg-emerald-500/10 text-emerald-700 dark:text-emerald-400',
        pending.has(value) &&
          'border-amber-500/30 bg-amber-500/10 text-amber-700 dark:text-amber-400',
        danger.has(value) &&
          'border-destructive/30 bg-destructive/10 text-destructive'
      )}
    >
      <Icon
        className={cn(
          'size-3',
          pending.has(value) && value !== 'pending' && 'animate-spin'
        )}
      />
      {labels[value] ?? value}
    </Badge>
  )
}
