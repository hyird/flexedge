import { QueryClient, QueryObserver } from '@tanstack/react-query'
import { afterEach, expect, test } from 'vitest'
import {
  closeLiveQueries,
  readLiveQuery,
  setLiveQueryQueryClient,
} from '@/lib/live-query'
import { createLiveLogStore, type LogEntry } from './live-log-store'

class Source extends EventTarget {
  closed = false
  close() {
    this.closed = true
  }
}

const open: Source[] = []
const live = () => open.filter((source) => !source.closed).length
const peaks: number[] = []
function trackedSource() {
  const source = new Source()
  open.push(source)
  peaks.push(live())
  return source
}
const parseLogs = (): LogEntry[] => []
const client = new QueryClient({
  defaultOptions: { queries: { retry: false, staleTime: Infinity } },
})
setLiveQueryQueryClient(client, undefined, trackedSource)

afterEach(() => {
  closeLiveQueries()
  client.clear()
  open.splice(0)
  peaks.splice(0)
})

function watchDetail() {
  const key = ['nodes', 'n1', 'detail']
  const observer = new QueryObserver(client, {
    queryKey: key,
    queryFn: ({ queryKey, signal }) =>
      readLiveQuery('/api/nodes/n1/stream', queryKey, signal),
  })
  const unsubscribe = observer.subscribe(() => undefined)
  return () => {
    unsubscribe()
    observer.destroy()
  }
}
// Pending releases run in microtasks; draining them needs no wall-clock wait.
const settled = async () => {
  for (let turn = 0; turn < 3; turn++) await Promise.resolve()
}

test('switching from a live detail to its log tail in one commit never holds two streams', async () => {
  const leaveDetail = watchDetail()
  await settled()
  expect(live()).toBe(1)
  // Same commit: the detail tab unmounts, the log tab mounts.
  leaveDetail()
  const logs = createLiveLogStore(
    '/api/nodes/n1/logs/stream',
    parseLogs,
    trackedSource
  )
  const stopLogs = logs.subscribe(() => undefined)
  expect(live()).toBe(1)
  expect(Math.max(...peaks)).toBe(1)
  stopLogs()
})

test('switching from the log tail back to the live detail never holds two streams', async () => {
  const logs = createLiveLogStore(
    '/api/nodes/n1/logs/stream',
    parseLogs,
    trackedSource
  )
  const stopLogs = logs.subscribe(() => undefined)
  expect(live()).toBe(1)
  stopLogs()
  const leaveDetail = watchDetail()
  await settled()
  expect(live()).toBe(1)
  expect(Math.max(...peaks)).toBe(1)
  leaveDetail()
})
