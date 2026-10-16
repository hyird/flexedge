import { Tooltip } from '@heroui/react'
import { useDisplayNow } from '@/lib/display-clock'
import { describeStatus } from '@/lib/status'
import { StatusChip } from '@/components/status-chip'
import {
  connectionStatusReserve,
  connectionStatuses,
  heartbeatSummary,
} from './node-display'
import type { Node } from './types'

/** The node's one online state (在线 / 离线 / 未接入) on a single line of
 * reserved width; the latest heartbeat is in its tooltip. The label follows
 * snapshots, the tooltip follows `node-runtime` heartbeats. */
export function NodeConnectionChip({
  runtime,
}: {
  runtime: Pick<Node['runtime'], 'connection_status' | 'last_heartbeat_at'>
}) {
  const now = useDisplayNow()
  const summary = heartbeatSummary(runtime, now)
  const status = describeStatus(connectionStatuses, runtime.connection_status)
  return (
    <Tooltip delay={300}>
      <Tooltip.Trigger
        aria-label={`${status.label}，${summary}`}
        className='inline-flex shrink-0 rounded-full align-middle'
      >
        <StatusChip {...status} reserve={connectionStatusReserve} />
      </Tooltip.Trigger>
      <Tooltip.Content>{summary}</Tooltip.Content>
    </Tooltip>
  )
}
