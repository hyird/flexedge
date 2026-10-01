import { describe, expect, it } from 'vitest'
import { canCancelCacheJob, canRetryCacheJob } from './cache-job-actions'
import type { CacheJobNode } from './cache-job-types'

function nodes(...statuses: string[]): CacheJobNode[] {
  return statuses.map((status, index) => ({
    node_id: String(index),
    node_name: `Node ${index}`,
    status,
    results: [],
  }))
}

describe('cache job action availability', () => {
  it('offers cancellation only for active preheat jobs', () => {
    for (const status of ['queued', 'running']) {
      expect(canCancelCacheJob({ operation: 'preheat', status })).toBe(true)
      expect(canCancelCacheJob({ operation: 'purge', status })).toBe(false)
    }
    for (const status of ['cancelling', 'completed', 'failed', 'cancelled'])
      expect(canCancelCacheJob({ operation: 'preheat', status })).toBe(false)
  })
  it('offers retry only after completion with a failed or cancelled node', () => {
    for (const status of ['completed', 'failed', 'cancelled']) {
      expect(
        canRetryCacheJob({ status, nodes: nodes('completed', 'failed') })
      ).toBe(true)
      expect(
        canRetryCacheJob({ status, nodes: nodes('completed', 'cancelled') })
      ).toBe(true)
      expect(
        canRetryCacheJob({ status, nodes: nodes('completed', 'completed') })
      ).toBe(false)
      expect(canRetryCacheJob({ status, nodes: [] })).toBe(false)
    }
    for (const status of ['queued', 'running', 'cancelling'])
      expect(
        canRetryCacheJob({ status, nodes: nodes('completed', 'failed') })
      ).toBe(false)
  })
  it('uses node failure status rather than individual URL results for retry eligibility', () => {
    const completedNodes = nodes('completed')
    completedNodes[0].results = [
      { target: 'https://example.com/a', status: 'failed' },
    ]
    expect(
      canRetryCacheJob({ status: 'completed', nodes: completedNodes })
    ).toBe(false)
    expect(canRetryCacheJob({ status: 'failed', nodes: nodes('failed') })).toBe(
      true
    )
  })
})
