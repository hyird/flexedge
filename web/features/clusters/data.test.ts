import { QueryClient, QueryObserver } from '@tanstack/react-query'
import { afterEach, expect, test } from 'vitest'
import { setLiveQueryQueryClient } from '@/lib/live-query'
import { clusterOptionsQuery } from './data'

class Source extends EventTarget { close() {} }
const client = new QueryClient({ defaultOptions: { queries: { retry: false } } })
afterEach(() => { setLiveQueryQueryClient(client); client.clear() })

test('cluster options keep a full collection and apply subsequent snapshots', async () => {
  const source = new Source()
  setLiveQueryQueryClient(client, undefined, () => source as never)
  const observer = new QueryObserver(client, clusterOptionsQuery)
  const stop = observer.subscribe(() => undefined)
  const result = observer.refetch()
  source.dispatchEvent(new MessageEvent('snapshot', {
    data: JSON.stringify({ code: 0, message: 'ok', data: Array.from({ length: 1001 }, (_, i) => ({ id: String(i) })) }),
  }))
  await expect(result).resolves.toMatchObject({ data: expect.any(Array) })
  expect(client.getQueryData(clusterOptionsQuery.queryKey)).toHaveLength(1001)
  source.dispatchEvent(new MessageEvent('snapshot', {
    data: JSON.stringify({ code: 0, message: 'ok', data: [{ id: 'replacement' }] }),
  }))
  await new Promise<void>((resolve) => queueMicrotask(resolve))
  expect(client.getQueryData(clusterOptionsQuery.queryKey)).toEqual([{ id: 'replacement' }])
  stop()
  observer.destroy()
})
