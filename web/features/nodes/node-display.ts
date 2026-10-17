import { formatRelative, formatUtcDate } from '@/lib/format'
import {
  enablementStatuses,
  statusReserve,
  type StatusDictionary,
} from '@/lib/status'
import type { Node } from './types'

/** `runtime.connection_status`: the one combined online state defined by
 * service/features/node_presence/node_connection.h (registered, enabled and heard from
 * within the online window). The latest heartbeat sits in its tooltip. */
export const connectionStatuses = {
  online: { label: '在线', tone: 'success' },
  offline: { label: '离线', tone: 'danger' },
  unregistered: { label: '未接入', tone: 'default' },
} as const satisfies StatusDictionary<'online' | 'offline' | 'unregistered'>
export const connectionStatusReserve = statusReserve(connectionStatuses)

/** Node `status`: the shared enabled/disabled switch. */
export const nodeStatuses = enablementStatuses
export const nodeStatusReserve = statusReserve(nodeStatuses)

/** `runtime.health` as node/control/control_channel.h reports it; other
 * agent-reported values keep their raw text. */
export const nodeHealthStatuses = {
  healthy: { label: '健康', tone: 'success' },
  degraded: { label: '降级', tone: 'warning' },
} as const satisfies StatusDictionary<'healthy' | 'degraded'>
export const nodeHealthReserve = statusReserve(nodeHealthStatuses)

/** Whether the node runs its target spec (never the revision numbers). */
export const nodeSpecStates = {
  applied: { label: '已生效', tone: 'success' },
  pending: { label: '待生效', tone: 'warning' },
} as const satisfies StatusDictionary<'applied' | 'pending'>
export const nodeSpecStateReserve = statusReserve(nodeSpecStates)

type LiveMetrics = Pick<
  Node['runtime'],
  | 'cpu_usage'
  | 'memory_usage'
  | 'disk_usage'
  | 'traffic_out_bps'
  | 'connection_count'
  | 'connected_ip_count'
  | 'load_1m'
  | 'queued_log_events'
  | 'dropped_log_events'
  | 'health'
>

/** Heartbeat metrics only describe a node that is online now; an offline or
 * unregistered node keeps its last report, which must read as «—». */
export function liveMetrics(runtime: Node['runtime']): LiveMetrics {
  if (runtime.connection_status !== 'online') return {}
  return {
    cpu_usage: runtime.cpu_usage,
    memory_usage: runtime.memory_usage,
    disk_usage: runtime.disk_usage,
    traffic_out_bps: runtime.traffic_out_bps,
    connection_count: runtime.connection_count,
    connected_ip_count: runtime.connected_ip_count,
    load_1m: runtime.load_1m,
    queued_log_events: runtime.queued_log_events,
    dropped_log_events: runtime.dropped_log_events,
    health: runtime.health,
  }
}

/** Tooltip text for the online state, e.g. `最近心跳 刚刚（2026-10-14 08:00:05 UTC）`. */
export function heartbeatSummary(
  runtime: Pick<Node['runtime'], 'last_heartbeat_at'>,
  now: number
) {
  const value = runtime.last_heartbeat_at
  if (!value) return '尚无心跳'
  return `最近心跳 ${formatRelative(value, now)}（${formatUtcDate(value)}）`
}
