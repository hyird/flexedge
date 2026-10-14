import { formatRelative, formatUtcDate } from '@/lib/format'
import { statusLabels } from '@/lib/status'
import type { Node } from './types'

// Every label each node status slot can show (`StatusChip` `reserve`), so a
// state change never moves neighbouring content. `connection_status` is the
// one combined online state (未接入 / 在线 / 离线) defined by
// service/domains/node/node_connection.h: registered, enabled and heard from
// within the online window. The latest heartbeat sits in its tooltip.
export const connectionLabels = statusLabels([
  'online',
  'offline',
  'unregistered',
])
export const nodeStatusLabels = statusLabels(['enabled', 'disabled'])
// node/control/control_channel.h reports `healthy` or `degraded`.
export const nodeHealthStatuses = ['healthy', 'degraded']
export const nodeHealthLabels = statusLabels(nodeHealthStatuses)

type LiveMetrics = Pick<
  Node['runtime'],
  | 'cpu_usage'
  | 'memory_usage'
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
