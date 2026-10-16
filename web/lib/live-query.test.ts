import {
  QueryClient,
  QueryObserver,
  type QueryKey,
} from '@tanstack/react-query'
import { afterEach, describe, expect, test, vi } from 'vitest'
import { ApiBusinessError, ApiProtocolError } from './api-response'
import { LiveQueryManager, type RecoverSession } from './live-query'
import { minimumLoadingMs } from './minimum-loading'

class Source extends EventTarget {
  closed = false
  close() {
    this.closed = true
  }
  frame(name: string, data: unknown) {
    this.dispatchEvent(new MessageEvent(name, { data: JSON.stringify(data) }))
  }
  snapshot(data: unknown, event = 'snapshot') {
    this.frame(event, { code: 0, message: 'ok', data })
  }
}

/** Runs the microtasks a manager reconcile and query settling schedule. */
async function flush() {
  for (let turn = 0; turn < 5; turn++) await Promise.resolve()
}

const cleanups: Array<() => void> = []
afterEach(() => {
  cleanups
    .splice(0)
    .reverse()
    .forEach((cleanup) => cleanup())
})

type WatchOptions<T> = {
  event?: string
  patch?: (current: T | undefined, value: unknown) => T | undefined
  merge?: (current: T | undefined, frame: T) => T
}

function setup(recover: RecoverSession = vi.fn(async () => true), minimum = 0) {
  const client = new QueryClient({
    defaultOptions: { queries: { retry: false, staleTime: Infinity } },
  })
  const sources: Source[] = []
  const urls: string[] = []
  const manager = new LiveQueryManager(
    client,
    (url) => {
      urls.push(url)
      const source = new Source()
      sources.push(source)
      return source
    },
    recover,
    minimum
  )
  cleanups.push(() => {
    manager.dispose()
    client.clear()
  })
  const watch = <T = unknown>(
    key: QueryKey = ['nodes'],
    url = '/api/nodes/stream',
    { event = 'snapshot', patch, merge }: WatchOptions<T> = {}
  ) => {
    const observer = new QueryObserver(client, {
      queryKey: key,
      queryFn: ({ signal }) =>
        manager.bind<T>(url, event, key, signal, { patch, merge }),
    })
    const unsubscribe = observer.subscribe(() => undefined)
    cleanups.push(() => {
      unsubscribe()
      observer.destroy()
    })
    return { observer, unsubscribe }
  }
  const state = (key: QueryKey = ['nodes']) =>
    client.getQueryCache().find({ queryKey: key, exact: true })?.state
  return { client, sources, urls, manager, watch, state }
}

test('a combined stream feeds several queries over one connection until the last view leaves', async () => {
  const { watch, sources, client } = setup()
  const summary = watch(['overview', 'summary'], '/api/overview/stream')
  const traffic = watch(['overview', 'traffic'], '/api/overview/stream', {
    event: 'traffic',
  })
  expect(sources).toHaveLength(1)
  sources[0].snapshot({ count: 1 })
  sources[0].snapshot({ bytes: 2 }, 'traffic')
  expect(client.getQueryData(['overview', 'summary'])).toEqual({ count: 1 })
  expect(client.getQueryData(['overview', 'traffic'])).toEqual({ bytes: 2 })
  summary.unsubscribe()
  await flush()
  expect(sources[0].closed).toBe(false)
  traffic.unsubscribe()
  await flush()
  expect(sources[0].closed).toBe(true)
})

test('a view opened over another switches the page connection and switches back when it closes', async () => {
  const { watch, sources, urls, client } = setup()
  watch(['tasks', 'list'], '/api/tasks/stream')
  sources[0].snapshot(['list'])
  const sheet = watch(['tasks', 'detail'], '/api/tasks/website/a/stream')
  expect(sources[0].closed).toBe(true)
  expect(sources.filter((source) => !source.closed)).toHaveLength(1)
  sources[1].snapshot({ id: 'a' })
  // A retired connection can no longer write its view.
  sources[0].snapshot(['stale'])
  expect(client.getQueryData(['tasks', 'list'])).toEqual(['list'])
  expect(client.getQueryData(['tasks', 'detail'])).toEqual({ id: 'a' })
  sheet.unsubscribe()
  await flush()
  expect(sources[1].closed).toBe(true)
  expect(urls).toEqual([
    '/api/tasks/stream',
    '/api/tasks/website/a/stream',
    '/api/tasks/stream',
  ])
  // The underlying view receives its recovery snapshot.
  sources[2].snapshot(['recovered'])
  expect(client.getQueryData(['tasks', 'list'])).toEqual(['recovered'])
})

test('a StrictMode remount keeps one connection and the final unmount releases it', async () => {
  const { watch, sources } = setup()
  const first = watch()
  first.unsubscribe()
  const second = watch()
  await flush()
  expect(sources).toHaveLength(1)
  expect(sources[0].closed).toBe(false)
  second.unsubscribe()
  await flush()
  expect(sources[0].closed).toBe(true)
})

test('views of one stream share it; a cached remount reattaches with a new connection', async () => {
  const { watch, sources, client } = setup()
  const first = watch()
  const second = watch()
  sources[0].snapshot([1])
  sources[0].snapshot([2])
  expect(client.getQueryData(['nodes'])).toEqual([2])
  first.unsubscribe()
  second.unsubscribe()
  await flush()
  expect(sources[0].closed).toBe(true)
  watch()
  await flush()
  expect(sources).toHaveLength(2)
  sources[1].snapshot([3])
  expect(client.getQueryData(['nodes'])).toEqual([3])
})

test('a manual refetch reconnects and waits for a fresh first frame', async () => {
  const { watch, sources, client } = setup()
  const { observer } = watch()
  sources[0].snapshot([1])
  await flush()
  const refresh = observer.refetch()
  expect(sources).toHaveLength(2)
  expect(sources[0].closed).toBe(true)
  sources[1].snapshot([2])
  await refresh
  expect(client.getQueryData(['nodes'])).toEqual([2])
})

test('patch events update the frame without a new snapshot or connection', () => {
  const { watch, sources, client } = setup()
  watch<{ cpu: number }>(['nodes'], '/api/nodes/stream', {
    patch: (current, value) =>
      current && typeof value === 'object' && value && 'cpu' in value
        ? { cpu: Number(value.cpu) }
        : undefined,
  })
  sources[0].snapshot({ cpu: 1 })
  sources[0].frame('node-runtime', { cpu: 2 })
  sources[0].frame('node-runtime', { unrelated: true })
  expect(client.getQueryData(['nodes'])).toEqual({ cpu: 2 })
  expect(sources).toHaveLength(1)
})

test('append views fold each frame into their data', () => {
  const { watch, sources, client } = setup()
  watch<number[]>(['nodes', 'logs'], '/api/nodes/n/logs/stream?limit=100', {
    event: 'logs',
    merge: (current, frame) => [...(current ?? []), ...frame],
  })
  sources[0].snapshot([1], 'logs')
  sources[0].snapshot([2, 3], 'logs')
  expect(client.getQueryData(['nodes', 'logs'])).toEqual([1, 2, 3])
})

test('invalid frames keep the last data and set an error; business errors surface', async () => {
  const { watch, sources, state } = setup()
  watch()
  sources[0].snapshot([1])
  sources[0].dispatchEvent(new MessageEvent('snapshot', { data: '{' }))
  expect(state()?.data).toEqual([1])
  expect(state()?.error).toBeInstanceOf(ApiProtocolError)
  sources[0].frame('resource-error', { code: 16501, message: '节点不存在' })
  expect(state()?.error).toBeInstanceOf(ApiBusinessError)
  sources[0].snapshot([2])
  expect(state()?.status).toBe('success')
  expect(state()?.data).toEqual([2])
})

test('an interruption keeps the data and reports the reconnect', () => {
  const { watch, sources, state } = setup()
  watch()
  sources[0].snapshot([1])
  sources[0].dispatchEvent(new Event('error'))
  expect(state()?.data).toEqual([1])
  expect(state()?.error?.message).toBe('实时连接已中断，正在重新连接')
  expect(sources[0].closed).toBe(false)
})

test('an exhausted stream stops and a manual refetch reconnects it', async () => {
  const { watch, sources, state } = setup()
  const { observer } = watch()
  sources[0].dispatchEvent(new Event('exhausted'))
  await flush()
  expect(sources[0].closed).toBe(true)
  expect(state()?.error?.message).toBe('实时数据暂不可用，请稍后手动重试')
  void observer.refetch().catch(() => undefined)
  expect(sources).toHaveLength(2)
})

test('disabled views do not retain their stream', async () => {
  const { watch, sources } = setup()
  const { observer } = watch()
  observer.setOptions({ ...observer.options, enabled: false })
  await flush()
  expect(sources[0].closed).toBe(true)
})

test('session expiry recovers once, then reconnects the page stream', async () => {
  let finish!: (valid: boolean) => void
  const recover = vi.fn(
    () =>
      new Promise<boolean>((resolve) => {
        finish = resolve
      })
  )
  const { watch, sources } = setup(recover)
  watch()
  sources[0].frame('session-expired', {})
  sources[0].frame('session-expired', {})
  expect(recover).toHaveBeenCalledTimes(1)
  expect(sources[0].closed).toBe(true)
  finish(true)
  await flush()
  expect(sources).toHaveLength(2)
  expect(sources[1].closed).toBe(false)
})

test('an ended session or a closed manager never revives the stream', async () => {
  let finish!: (valid: boolean) => void
  const recover = vi.fn(
    () =>
      new Promise<boolean>((resolve) => {
        finish = resolve
      })
  )
  const { watch, sources, manager } = setup(recover)
  watch()
  sources[0].frame('session-expired', {})
  finish(false)
  await flush()
  expect(sources).toHaveLength(1)

  const other = setup(recover)
  other.watch()
  other.sources[0].frame('session-expired', {})
  other.manager.close()
  finish(true)
  await flush()
  expect(other.sources).toHaveLength(1)
  manager.close()
})

test('however many live views a page opens, at most one connection is open', async () => {
  const { watch, sources } = setup()
  const open = () => sources.filter((source) => !source.closed)
  const views = [
    watch(['overview', 'summary'], '/api/overview/stream'),
    watch(['tasks', 'list'], '/api/tasks/stream'),
    watch(['tasks', 'detail'], '/api/tasks/website/a/stream'),
    watch(['tasks', 'records'], '/api/tasks/website/a/stream', {
      event: 'history',
    }),
    watch(['nodes'], '/api/nodes/stream'),
  ]
  expect(open()).toHaveLength(1)
  for (const view of views.reverse()) {
    view.unsubscribe()
    await flush()
    expect(open().length).toBeLessThanOrEqual(1)
  }
  expect(open()).toHaveLength(0)
})

describe('minimum loading time of first loads', () => {
  afterEach(() => {
    vi.useRealTimers()
  })

  // `advanceTimersByTimeAsync` is Vitest-only; `bun test` runs the same suite.
  const advance = async (ms: number) => {
    await flush()
    vi.advanceTimersByTime(ms)
    await flush()
  }

  test('an early first frame keeps every view loading for the full minimum', async () => {
    vi.useFakeTimers()
    const { sources, watch } = setup(undefined, minimumLoadingMs)
    const table = watch()
    const header = watch()
    await advance(30)
    sources[0].snapshot({ value: 1 })
    // A live update during the hold replaces the waiting frame.
    sources[0].snapshot({ value: 2 })
    // Fake-clock margins of 10 ms: `bun test` lets real milliseconds leak
    // into the faked `Date.now()`.
    await advance(minimumLoadingMs - 40)
    expect(table.observer.getCurrentResult().isPending).toBe(true)
    expect(header.observer.getCurrentResult().isPending).toBe(true)
    await advance(10)
    expect(table.observer.getCurrentResult().data).toEqual({ value: 2 })
    expect(header.observer.getCurrentResult().data).toEqual({ value: 2 })
    // Live frames after the first load are never delayed.
    sources[0].snapshot({ value: 3 })
    expect(table.observer.getCurrentResult().data).toEqual({ value: 3 })
  })

  test('a revisit with cached data renders at once', async () => {
    vi.useFakeTimers()
    const { sources, watch, client } = setup(undefined, minimumLoadingMs)
    const first = watch()
    sources[0].snapshot([1])
    await advance(minimumLoadingMs)
    first.unsubscribe()
    await flush()
    watch()
    await flush()
    sources[1].snapshot([2])
    expect(client.getQueryData(['nodes'])).toEqual([2])
  })
})
