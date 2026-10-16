import { describe, expect, it } from 'vitest'
import { heartbeatSummary, liveMetrics } from './node-display'
import type { Node } from './types'

describe('liveMetrics', () => {
  const runtime = (
    connection_status: Node['runtime']['connection_status']
  ): Node['runtime'] => ({
    registration_status: 'registered',
    connection_status,
    applied_node_spec_revision: 1,
    cpu_usage: 0.4,
    connected_ip_count: 0,
    health: 'degraded',
  })

  it('keeps an online node’s heartbeat metrics, including zero', () => {
    expect(liveMetrics(runtime('online'))).toMatchObject({
      cpu_usage: 0.4,
      connected_ip_count: 0,
      health: 'degraded',
    })
  })

  it('hides the last report of offline and unregistered nodes', () => {
    expect(liveMetrics(runtime('offline'))).toEqual({})
    expect(liveMetrics(runtime('unregistered'))).toEqual({})
  })
})

describe('heartbeatSummary', () => {
  const heartbeat = '2026-10-14T08:00:05Z'

  it('shows a fresh heartbeat as 刚刚 with its UTC time', () => {
    expect(
      heartbeatSummary(
        { last_heartbeat_at: heartbeat },
        Date.parse('2026-10-14T08:00:12Z')
      )
    ).toBe('最近心跳 刚刚（2026-10-14 08:00:05 UTC）')
  })

  it('shows an older heartbeat relative to the display clock', () => {
    expect(
      heartbeatSummary(
        { last_heartbeat_at: heartbeat },
        Date.parse('2026-10-14T08:03:05Z')
      )
    ).toBe('最近心跳 3分钟前（2026-10-14 08:00:05 UTC）')
  })

  it('says so when the node has never sent a heartbeat', () => {
    expect(heartbeatSummary({}, Date.parse(heartbeat))).toBe('尚无心跳')
  })
})
