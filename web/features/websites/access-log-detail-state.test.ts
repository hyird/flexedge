import { describe, expect, it } from 'vitest'
import { updateAccessLogDetailState } from './access-log-detail-state'
import type { AccessLog } from './access-log-schema'

const log = (id: string): AccessLog =>
  ({
    id,
    occurred_at: id,
    node_id: 'node',
    node_name: 'Node',
    protocol: 'https',
    method: 'GET',
    host: 'example.test',
    target: `/${id}`,
    status_code: 200,
    request_bytes: 0,
    response_bytes: 0,
    duration_ms: 1,
    request_body_truncated: false,
  }) as AccessLog

describe('access log detail freeze state', () => {
  it('freezes once on the first open and keeps the snapshot for multiple details', () => {
    const first = [log('one')]
    const opened = updateAccessLogDetailState(
      new Set(),
      'one',
      true,
      first,
      null
    )
    const second = updateAccessLogDetailState(
      opened.openDetailIds,
      'two',
      true,
      [log('two')],
      opened.frozenLiveLogs
    )

    expect(second.openDetailIds).toEqual(new Set(['one', 'two']))
    expect(second.frozenLiveLogs?.map((entry) => entry.id)).toEqual(['one'])
  })

  it('restores the live list only after the last detail closes', () => {
    const opened = updateAccessLogDetailState(
      new Set(['one', 'two']),
      'one',
      false,
      [log('new')],
      [log('one')]
    )
    expect(opened.openDetailIds).toEqual(new Set(['two']))
    expect(opened.frozenLiveLogs?.map((entry) => entry.id)).toEqual(['one'])

    const closed = updateAccessLogDetailState(
      opened.openDetailIds,
      'two',
      false,
      [log('new')],
      opened.frozenLiveLogs
    )
    expect(closed.openDetailIds).toEqual(new Set())
    expect(closed.frozenLiveLogs).toBeNull()
  })
})
