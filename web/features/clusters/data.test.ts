import { QueryClient, QueryObserver } from '@tanstack/react-query'
import { afterEach, expect, test } from 'vitest'
import { setLiveQueryQueryClient } from '@/lib/live-query'
import { clusterOptionsQuery } from './data'

class Source extends EventTarget {
  closed = false
  close() {
    this.closed = true
  }
}
const client = new QueryClient({
  defaultOptions: { queries: { retry: false } },
})
afterEach(() => {
  setLiveQueryQueryClient(client)
  client.clear()
})

test('cluster options retain the full snapshot and refresh only on request', async () => {
  const sources: Source[] = []
  setLiveQueryQueryClient(client, undefined, () => {
    const source = new Source()
    sources.push(source)
    return source as never
  })
  const observer = new QueryObserver(client, clusterOptionsQuery)
  const stop = observer.subscribe(() => undefined)
  const result = observer.refetch()
  sources[0].dispatchEvent(
    new MessageEvent('snapshot', {
      data: JSON.stringify({
        code: 0,
        message: 'ok',
        data: Array.from({ length: 1001 }, (_, i) => ({ id: String(i) })),
      }),
    })
  )
  await expect(result).resolves.toMatchObject({ data: expect.any(Array) })
  expect(client.getQueryData(clusterOptionsQuery.queryKey)).toHaveLength(1001)
  expect(sources[0].closed).toBe(true)
  sources[0].dispatchEvent(
    new MessageEvent('snapshot', {
      data: JSON.stringify({
        code: 0,
        message: 'ok',
        data: [{ id: 'late-frame' }],
      }),
    })
  )
  expect(client.getQueryData(clusterOptionsQuery.queryKey)).toHaveLength(1001)
  const refresh = observer.refetch()
  sources[1].dispatchEvent(
    new MessageEvent('snapshot', {
      data: JSON.stringify({
        code: 0,
        message: 'ok',
        data: [{ id: 'replacement' }],
      }),
    })
  )
  await refresh
  expect(client.getQueryData(clusterOptionsQuery.queryKey)).toEqual([
    { id: 'replacement' },
  ])
  stop()
  observer.destroy()
})
