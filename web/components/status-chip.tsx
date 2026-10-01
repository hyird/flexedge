import {
  CircleCheck,
  CircleExclamation,
  Clock,
  CircleMinus,
} from '@gravity-ui/icons'
import { Chip } from '@heroui/react'

const labels: Record<string, string> = {
  enabled: '已启用',
  disabled: '已停用',
  online: '在线',
  offline: '离线',
  connected: '已连接',
  disconnected: '未连接',
  registered: '已注册',
  unregistered: '未注册',
  pending: '等待中',
  queued: '排队中',
  running: '执行中',
  processing: '处理中',
  success: '成功',
  succeeded: '已完成',
  completed: '已完成',
  done: '已完成',
  failed: '失败',
  error: '异常',
  conflict: '冲突',
  valid: '有效',
  expired: '已过期',
  issuing: '签发中',
  renewing: '续期中',
  verified: '已验证',
  unverified: '未验证',
  idle: '空闲',
  synced: '已同步',
  syncing: '同步中',
  cancelled: '已取消',
  canceled: '已取消',
  partial: '部分完成',
  ready: '就绪',
  resolved: '解析正常',
  healthy: '健康',
  unhealthy: '不健康',
  retrying: '重试中',
  unknown: '未知',
}
export function StatusChip({
  status,
  label,
}: {
  status?: string
  label?: string
}) {
  const value = status ?? 'unknown'
  const success = [
    'enabled',
    'online',
    'connected',
    'registered',
    'success',
    'succeeded',
    'completed',
    'done',
    'valid',
    'verified',
    'synced',
    'ready',
    'resolved',
    'healthy',
  ].includes(value)
  const danger = [
    'failed',
    'error',
    'conflict',
    'expired',
    'unhealthy',
  ].includes(value)
  const warning = [
    'pending',
    'queued',
    'running',
    'processing',
    'issuing',
    'renewing',
    'syncing',
    'partial',
    'retrying',
  ].includes(value)
  const Icon = success
    ? CircleCheck
    : danger
      ? CircleExclamation
      : warning
        ? Clock
        : CircleMinus
  return (
    <Chip
      size='sm'
      variant='soft'
      color={
        success
          ? 'success'
          : danger
            ? 'danger'
            : warning
              ? 'warning'
              : 'default'
      }
    >
      <Icon className='size-3' />
      <Chip.Label>{label ?? labels[value] ?? value}</Chip.Label>
    </Chip>
  )
}
