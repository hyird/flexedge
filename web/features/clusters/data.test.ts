import { QueryClient, QueryObserver } from '@tanstack/react-query'
import { afterEach, expect, test } from 'vitest'
import { setLiveQueryQueryClient } from '@/lib/live-query'
import {
  clusterOptionsQuery,
  clusterSnapshotKey,
  clusterSnapshotQuery,
} from './data'

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

test('cluster detail header reads one snapshot and re-reads on invalidation', async () => {
  const sources: Source[] = []
  const urls: string[] = []
  setLiveQueryQueryClient(client, undefined, (url) => {
    const source = new Source()
    sources.push(source)
    urls.push(url)
    return source as never
  })
  const id = '22bc1a68-6db8-4c77-bd29-c11e563fa187'
  const frame = (name: string) =>
    new MessageEvent('snapshot', {
      data: JSON.stringify({ code: 0, message: 'ok', data: { id, name } }),
    })
  const observer = new QueryObserver(client, clusterSnapshotQuery(id))
  const stop = observer.subscribe(() => undefined)
  const first = observer.refetch()
  sources[0].dispatchEvent(frame('first'))
  await first
  expect(urls[0]).toBe(`/api/clusters/${id}/stream`)
  expect(sources[0].closed).toBe(true)
  const reread = client.invalidateQueries({ queryKey: clusterSnapshotKey })
  sources[1].dispatchEvent(frame('second'))
  await reread
  expect(sources[1].closed).toBe(true)
  expect(client.getQueryData(clusterSnapshotQuery(id).queryKey)).toMatchObject({
    name: 'second',
  })
  stop()
  observer.destroy()
})
