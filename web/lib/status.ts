export type StatusTone = 'success' | 'warning' | 'danger' | 'accent' | 'default'
// Wire status → [Chinese label, tone]. Tones carry meaning only together with
// the label text; never rely on color alone.
const statuses: Record<string, [label: string, tone: StatusTone]> = {
  enabled: ['已启用', 'success'],
  disabled: ['已停用', 'default'],
  active: ['正常', 'success'],
  online: ['在线', 'success'],
  offline: ['离线', 'danger'],
  connected: ['已连接', 'success'],
  disconnected: ['未连接', 'danger'],
  registered: ['已注册', 'success'],
  unregistered: ['未接入', 'default'],
  pending: ['等待中', 'warning'],
  queued: ['排队中', 'accent'],
  running: ['执行中', 'accent'],
  processing: ['处理中', 'accent'],
  retrying: ['重试中', 'warning'],
  retry: ['等待重试', 'warning'],
  success: ['成功', 'success'],
  succeeded: ['已完成', 'success'],
  completed: ['已完成', 'success'],
  done: ['已完成', 'success'],
  recovered: ['已恢复', 'success'],
  superseded: ['已结束', 'default'],
  failed: ['失败', 'danger'],
  error: ['异常', 'danger'],
  timeout: ['超时', 'danger'],
  conflict: ['冲突', 'danger'],
  valid: ['有效', 'success'],
  invalid: ['无效', 'danger'],
  expired: ['已过期', 'danger'],
  issuing: ['签发中', 'accent'],
  renewing: ['续期中', 'accent'],
  verified: ['已验证', 'success'],
  unverified: ['未验证', 'warning'],
  idle: ['空闲', 'default'],
  synced: ['已同步', 'success'],
  syncing: ['同步中', 'accent'],
  consistent: ['一致', 'success'],
  out_of_sync: ['未同步', 'warning'],
  applied: ['已生效', 'success'],
  no_nodes: ['无节点', 'default'],
  cancelled: ['已取消', 'default'],
  canceled: ['已取消', 'default'],
  partial: ['部分完成', 'warning'],
  ready: ['就绪', 'success'],
  resolved: ['解析正常', 'success'],
  healthy: ['健康', 'success'],
  degraded: ['降级', 'warning'],
  unhealthy: ['不健康', 'danger'],
  unknown: ['未知', 'default'],
}

/** Label and tone for a wire status; unknown values keep their raw text. */
export function describeStatus(status?: string): {
  label: string
  tone: StatusTone
} {
  const value = status || 'unknown'
  const [label, tone] = statuses[value] ?? [value, 'default']
  return { label, tone }
}

/** Labels of the given wire statuses, for `StatusChip`'s `reserve`. */
export function statusLabels(statuses: readonly string[]) {
  return statuses.map((status) => describeStatus(status).label)
}
