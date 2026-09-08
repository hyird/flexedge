import { QueryClient, QueryObserver } from '@tanstack/react-query'
import { expect, test } from 'vitest'
import {
  queryKeysForSyncEvents,
  syncEventRefreshKeys,
  syncEventFailureMessage,
  refreshSyncEventQueries,
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

test('completion waits for the website list response to replace stale data', async () => {
  const client = new QueryClient({
    defaultOptions: { queries: { retry: false, staleTime: Infinity } },
  })
  let respond!: (value: string[]) => void
  const observer = new QueryObserver(client, {
    queryKey: ['websites', 1],
    initialData: ['old'],
    queryFn: () =>
      new Promise<string[]>((resolve) => {
        respond = resolve
      }),
  })
  const unsubscribe = observer.subscribe(() => {})
  try {
    let completed = false
    const refresh = refreshSyncEventQueries(client, [
      event({ resource_type: 'website' }),
    ]).then(() => {
      completed = true
    })
    await Promise.resolve()
    expect(completed).toBe(false)
    expect(observer.getCurrentResult().data).toEqual(['old'])
    respond(['new'])
    await refresh
    expect(observer.getCurrentResult().data).toEqual(['new'])
    expect(completed).toBe(true)
  } finally {
    unsubscribe()
    client.clear()
  }
})

test('failed list refresh cannot be reported as a successful refresh', async () => {
  const client = new QueryClient({
    defaultOptions: { queries: { retry: false, staleTime: Infinity } },
  })
  const observer = new QueryObserver(client, {
    queryKey: ['websites', 1],
    initialData: ['old'],
    queryFn: async () => {
      throw new Error('network failure')
    },
  })
  const unsubscribe = observer.subscribe(() => {})
  try {
    await expect(
      refreshSyncEventQueries(client, [event({ resource_type: 'website' })])
    ).rejects.toThrow('network failure')
    expect(observer.getCurrentResult().data).toEqual(['old'])
  } finally {
    unsubscribe()
    client.clear()
  }
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

test('failure wording names the actual website background operation', () => {
  expect(
    syncEventFailureMessage(
      event({ resource_type: 'website', outcome: 'failed' })
    )
  ).toBe('网站域名检查失败，请查看资源详情')
})
