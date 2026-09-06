import { expect, test } from 'vitest'
import {
  queryKeysForSyncEvents,
  syncEventRefreshKeys,
  syncEventCompletionMessage,
} from '@/lib/sync-events'
import type { SyncEvent } from '@/lib/types'

const event = (overrides: Partial<SyncEvent> = {}): SyncEvent => ({
  sequence: 1,
  resource_type: 'dns_zone',
  resource_id: 'zone-1',
  operation: 'sync',
  version: 1,
  outcome: 'completed',
  emitted_at: '2026-09-06T20:00:00+08:00',
  ...overrides,
})

test('every worker-backed resource type has an explicit refresh contract', () => {
  expect(Object.keys(syncEventRefreshKeys).sort()).toEqual([
    'certificate',
    'dns_zone',
    'provider',
    'website',
  ])
})

test('a DNS result refreshes its direct and dependent resource pages', () => {
  expect(queryKeysForSyncEvents([event()])).toEqual([
    ['overview'],
    ['dns-zones'],
    ['clusters'],
    ['nodes'],
  ])
})

test('a batched result refreshes each query family once', () => {
  expect(
    queryKeysForSyncEvents([
      event({ resource_type: 'certificate' }),
      event({ resource_type: 'certificate', sequence: 2 }),
    ])
  ).toEqual([['overview'], ['certificates'], ['websites']])
})

test('completion wording names the resource category after its refresh', () => {
  expect(syncEventCompletionMessage(event({ resource_type: 'website' }))).toBe(
    '网站发布已完成，相关数据已刷新'
  )
})
