import {
  keepPreviousData,
  QueryClient,
  QueryObserver,
  type QueryKey,
} from '@tanstack/react-query'
import { afterEach, describe, expect, test, vi } from 'vitest'
import { ApiBusinessError, ApiProtocolError } from './api-response'
import { reconnectingEventSource } from './event-stream'
import { LiveQueryManager } from './live-query'
import { LoadingEpoch, minimumLoadingMs } from './minimum-loading'

class Source extends EventTarget {
  closed = false
  close() {
    this.closed = true
  }
  frame(name: string, data: unknown) {
    this.dispatchEvent(new MessageEvent(name, { data: JSON.stringify(data) }))
  }
  snapshot(data: unknown) {
    this.frame('snapshot', { code: 0, message: 'ok', data })
  }
}

async function settled(check: () => void) {
  await new Promise((resolve) => setTimeout(resolve, 0))
  check()
}

const cleanups: Array<() => void> = []
test('configuration options consume one snapshot and release their connection while observed', async () => {
  const { watch, sources, client, manager } = setup()
  const options = watch(['clusters', 'options'], '/api/clusters/options/stream')
  sources[0].snapshot([{ id: 'cluster-a' }])
  await settled(() => {
    expect(sources[0].closed).toBe(true)
    expect(client.getQueryData(['clusters', 'options'])).toEqual([
      { id: 'cluster-a' },
    ])
  })
  options.unsubscribe()
  const again = watch(['clusters', 'options'], '/api/clusters/options/stream')
  await settled(() => expect(sources).toHaveLength(1))
  void client.invalidateQueries({ queryKey: ['clusters', 'options'] })
  await settled(() => expect(sources).toHaveLength(2))
  sources[1].snapshot([{ id: 'cluster-b' }])
  await settled(() => {
    expect(sources[1].closed).toBe(true)
    expect(again.observer.getCurrentResult().data).toEqual([
      { id: 'cluster-b' },
    ])
  })
  manager.close()
})
afterEach(() => {
  cleanups
    .splice(0)
    .reverse()
    .forEach((cleanup) => cleanup())
})

test('historical access logs release their stream after a snapshot even without history in their key', async () => {
  const { watch, sources, client } = setup()
  const key = ['websites', 7, 'access-logs', { page: 1 }]
  watch(key, '/api/websites/7/access-logs/history/stream?page=1')
  const records = { items: [{ id: 'historical-record' }], total: 1 }
  sources[0].snapshot(records)
  await settled(() => {
    expect(client.getQueryData(key)).toEqual(records)
    expect(sources[0].closed).toBe(true)
  })
  sources[0].snapshot({ items: [], total: 0 })
  expect(client.getQueryData(key)).toEqual(records)
})

test('explicit one-shot reads release a list stream after the first snapshot while still observed', async () => {
  const { sources, client, manager } = setup()
  const key = ['websites', 'command-search', 'qa']
  const observer = new QueryObserver(client, {
    queryKey: key,
    queryFn: ({ queryKey, signal }) =>
      manager.readSnapshot(
        '/api/websites/stream?keyword=qa&page=1&page_size=5',
        queryKey,
        signal
      ),
  })
  const unsubscribe = observer.subscribe(() => undefined)
  cleanups.push(() => {
    unsubscribe()
    observer.destroy()
  })
  sources[0].snapshot({ list: [{ id: 'website-a' }], total: 1 })
  await settled(() => {
    expect(sources[0].closed).toBe(true)
    expect(observer.getCurrentResult().data).toEqual({
      list: [{ id: 'website-a' }],
      total: 1,
    })
  })
  sources[0].snapshot({ list: [], total: 0 })
  expect(client.getQueryData(key)).toEqual({
    list: [{ id: 'website-a' }],
    total: 1,
  })
  expect(sources).toHaveLength(1)
})

test('a combined stream feeds several query keys over one connection until the last observer leaves', async () => {
  const { sources, client, manager } = setup()
  const observe = (
    event: string,
    patch?: (current: unknown, value: unknown) => unknown
  ) => {
    const observer = new QueryObserver(client, {
      queryKey: ['overview', event],
      queryFn: ({ queryKey, signal }) =>
        manager.readEvent(
          '/api/overview/stream',
          event,
          queryKey,
          signal,
          undefined,
          patch
        ),
    })
    const unsubscribe = observer.subscribe(() => undefined)
    cleanups.push(() => observer.destroy())
    return { observer, unsubscribe }
  }
  const summary = observe('snapshot')
  const traffic = observe('traffic')
  const nodes = observe('nodes', (current, value) => ({
    ...(current as object),
    cpu: (value as { cpu: number }).cpu,
  }))
  await settled(() => expect(sources).toHaveLength(1))
  sources[0].snapshot({ websites: 3 })
  sources[0].frame('traffic', { code: 0, message: 'ok', data: { requests: 9 } })
  sources[0].frame('nodes', { code: 0, message: 'ok', data: { cpu: 0.1 } })
  await settled(() => {
    expect(summary.observer.getCurrentResult().data).toEqual({ websites: 3 })
    expect(traffic.observer.getCurrentResult().data).toEqual({ requests: 9 })
    expect(nodes.observer.getCurrentResult().data).toEqual({ cpu: 0.1 })
  })
  sources[0].frame('node-runtime', { cpu: 0.5 })
  sources[0].frame('traffic', {
    code: 0,
    message: 'ok',
    data: { requests: 10 },
  })
  await settled(() => {
    expect(nodes.observer.getCurrentResult().data).toEqual({ cpu: 0.5 })
    expect(traffic.observer.getCurrentResult().data).toEqual({ requests: 10 })
    expect(summary.observer.getCurrentResult().data).toEqual({ websites: 3 })
  })
  summary.unsubscribe()
  traffic.unsubscribe()
  await settled(() => expect(sources[0].closed).toBe(false))
  nodes.unsubscribe()
  await settled(() => {
    expect(sources[0].closed).toBe(true)
    expect(sources).toHaveLength(1)
  })
})

test('a resource error on a combined stream fails every bound query', async () => {
  const { sources, client, manager } = setup()
  const observers = ['snapshot', 'traffic'].map((event) => {
    const observer = new QueryObserver(client, {
      queryKey: ['overview', event],
      queryFn: ({ queryKey, signal }) =>
        manager.readEvent('/api/overview/stream', event, queryKey, signal),
    })
    const unsubscribe = observer.subscribe(() => undefined)
    cleanups.push(() => {
      unsubscribe()
      observer.destroy()
    })
    return observer
  })
  sources[0].frame('resource-error', { code: 16601, message: '网站不存在' })
  await settled(() => {
    for (const observer of observers)
      expect(observer.getCurrentResult().error).toBeInstanceOf(ApiBusinessError)
  })
})

test('switching views in one commit closes the old stream before the new one opens', async () => {
  const { watch, sources, urls } = setup()
  const open = () => sources.filter((source) => !source.closed).length
  const peak: number[] = []
  const nodes = watch(['nodes', 'n1', 'detail'], '/api/nodes/n1/stream')
  sources[0].snapshot({ id: 'n1' })
  await settled(() => expect(open()).toBe(1))
  // Same commit: the old observer unmounts, then the new one mounts.
  nodes.unsubscribe()
  watch(['nodes', 'n1', 'logs'], '/api/nodes/n1/logs-view/stream')
  peak.push(open())
  await settled(() => {
    peak.push(open())
    expect(urls).toEqual([
      '/api/nodes/n1/stream',
      '/api/nodes/n1/logs-view/stream',
    ])
    expect(sources[0].closed).toBe(true)
    expect(Math.max(...peak)).toBe(1)
  })
})

describe('combined stream release races', () => {
  const events = ['snapshot', 'traffic', 'nodes']
  function observeOverview(
    client: QueryClient,
    manager: LiveQueryManager,
    event: string
  ) {
    const observer = new QueryObserver(client, {
      queryKey: ['overview', event],
      queryFn: ({ queryKey, signal }) =>
        manager.readEvent('/api/overview/stream', event, queryKey, signal),
    })
    const unsubscribe = observer.subscribe(() => undefined)
    cleanups.push(() => observer.destroy())
    return unsubscribe
  }
  const frame = (source: Source, event: string) =>
    source.frame(event, { code: 0, message: 'ok', data: { event } })
  const openCount = (sources: Source[]) =>
    sources.filter((source) => !source.closed).length

  test('every unmount order before the first frame releases the connection', async () => {
    for (const order of [
      [0, 1, 2],
      [2, 1, 0],
      [1, 2, 0],
    ]) {
      const { sources, client, manager } = setup()
      const leave = events.map((event) =>
        observeOverview(client, manager, event)
      )
      for (const index of order) leave[index]()
      await settled(() => {
        expect(sources).toHaveLength(1)
        expect(openCount(sources)).toBe(0)
      })
    }
  })

  test('frames interleaved with unmounts still release after the last observer', async () => {
    const { sources, client, manager } = setup()
    const leave = events.map((event) => observeOverview(client, manager, event))
    frame(sources[0], 'traffic')
    leave[1]()
    sources[0].snapshot({ event: 'snapshot' })
    leave[0]()
    await settled(() => expect(openCount(sources)).toBe(1))
    frame(sources[0], 'nodes')
    leave[2]()
    frame(sources[0], 'traffic')
    await settled(() => {
      expect(openCount(sources)).toBe(0)
      expect(sources).toHaveLength(1)
    })
  })

  test('a StrictMode remount keeps one connection and the final unmount releases it', async () => {
    const { sources, client, manager } = setup()
    const first = events.map((event) => observeOverview(client, manager, event))
    // StrictMode: effects clean up and run again in the same turn.
    first.forEach((leave) => leave())
    const second = events.map((event) =>
      observeOverview(client, manager, event)
    )
    await settled(() => {
      expect(sources).toHaveLength(1)
      expect(openCount(sources)).toBe(1)
    })
    second.forEach((leave) => leave())
    await settled(() => expect(openCount(sources)).toBe(0))
  })

  test('a flush from another view or a session reconnect never revives a departed stream', async () => {
    const recover = vi.fn(async () => ({}))
    const { sources, client, manager } = setup(recover)
    const leave = events.map((event) => observeOverview(client, manager, event))
    sources[0].snapshot({ event: 'snapshot' })
    // Session recovery is pending while the page unmounts.
    sources[0].frame('session-expired', {})
    leave.forEach((unsubscribe) => unsubscribe())
    // The recovery reconnect runs before the queued release microtasks; its
    // flush retires the departed entry, which must not open a new source.
    await settled(() => {
      expect(recover).toHaveBeenCalledTimes(1)
      expect(openCount(sources)).toBe(0)
    })
  })
})

test('the default resource transport does not reconnect idle snapshots on a timer', async () => {
  const client = new QueryClient({
    defaultOptions: { queries: { retry: false } },
  })
  const source = new Source()
  const create = vi.fn(function () {
    return source
  })
  const originalSource = Object.getOwnPropertyDescriptor(
    globalThis,
    'EventSource'
  )
  Object.defineProperty(globalThis, 'EventSource', {
    configurable: true,
    writable: true,
    value: create,
  })
  const timer = vi.spyOn(globalThis, 'setTimeout')
  const manager = new LiveQueryManager(client)
  try {
    const pending = manager.read('/api/nodes/stream', ['nodes'])
    source.snapshot([])
    await pending
    expect(timer.mock.calls.some((call) => call[1] === 45000)).toBe(false)
    expect(create).toHaveBeenCalledTimes(1)
  } finally {
    manager.dispose()
    client.clear()
    timer.mockRestore()
    if (originalSource)
      Object.defineProperty(globalThis, 'EventSource', originalSource)
    else Reflect.deleteProperty(globalThis, 'EventSource')
  }
})

function setup(recover = vi.fn(async () => ({})), onSessionError = vi.fn()) {
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
    onSessionError
  )
  cleanups.push(() => {
    manager.dispose()
    client.clear()
  })
  const watch = <T>(
    key: QueryKey = ['nodes'],
    url = '/api/nodes/stream',
    parse?: (value: unknown) => T,
    patch?: (current: T | undefined, value: unknown) => T | undefined
  ) => {
    const observer = new QueryObserver(client, {
      queryKey: key,
      queryFn: ({ signal }) => manager.read<T>(url, key, signal, parse, patch),
    })
    const unsubscribe = observer.subscribe(() => undefined)
    cleanups.push(() => {
      unsubscribe()
      observer.destroy()
    })
    return { observer, unsubscribe }
  }
  return { client, sources, urls, manager, watch, recover, onSessionError }
}

test('subsequent snapshots update the cache without additional connections', async () => {
  const { watch, sources, client } = setup()
  watch<{ value: number }>()
  sources[0].snapshot({ value: 1 })
  await settled(() =>
    expect(client.getQueryData(['nodes'])).toEqual({ value: 1 })
  )
  sources[0].snapshot({ value: 2 })
  await settled(() =>
    expect(client.getQueryData(['nodes'])).toEqual({ value: 2 })
  )
  sources[0].frame('heartbeat', {})
  expect(sources).toHaveLength(1)
  expect(sources[0].closed).toBe(false)
})

test('observers share a stream and the final unmount closes it; cached remount reopens', async () => {
  const { watch, sources } = setup()
  const first = watch()
  const second = watch()
  sources[0].snapshot([])
  await settled(() =>
    expect(first.observer.getCurrentResult().isSuccess).toBe(true)
  )
  first.unsubscribe()
  await Promise.resolve()
  expect(sources[0].closed).toBe(false)
  second.unsubscribe()
  await Promise.resolve()
  expect(sources[0].closed).toBe(true)
  watch()
  expect(sources).toHaveLength(2)
})

test('parameter switches release the old stream and late frames cannot overwrite new data', async () => {
  const { watch, sources, client } = setup()
  const first = watch(['nodes', 1], '/api/nodes/stream?page=1')
  sources[0].snapshot(['page1'])
  await Promise.resolve()
  first.unsubscribe()
  watch(['nodes', 2], '/api/nodes/stream?page=2')
  await Promise.resolve()
  expect(sources[0].closed).toBe(true)
  sources[1].snapshot(['page2'])
  sources[0].snapshot(['stale'])
  await settled(() =>
    expect(client.getQueryData(['nodes', 2])).toEqual(['page2'])
  )
  expect(client.getQueryData(['nodes', 1])).toEqual(['page1'])
})

test('manual refresh reconnects and waits for a new first snapshot', async () => {
  const { watch, sources } = setup()
  const { observer } = watch<number>()
  sources[0].snapshot(1)
  await settled(() => expect(observer.getCurrentResult().data).toBe(1))
  const pending = observer.refetch()
  expect(sources[0].closed).toBe(true)
  expect(sources).toHaveLength(2)
  expect(observer.getCurrentResult().isFetching).toBe(true)
  sources[0].snapshot(99)
  sources[1].snapshot(2)
  expect((await pending).data).toBe(2)
})

test('runtime frames update node data without a new snapshot or connection', async () => {
  const { watch, sources, client } = setup()
  watch<{ count: number }>(
    ['nodes'],
    '/api/nodes/stream',
    undefined,
    (current, value) =>
      current ? { count: current.count + Number(value) } : current
  )
  sources[0].snapshot({ count: 2 })
  await settled(() =>
    expect(client.getQueryData(['nodes'])).toEqual({ count: 2 })
  )
  sources[0].frame('node-runtime', 3)
  expect(client.getQueryData(['nodes'])).toEqual({ count: 5 })
  expect(sources).toHaveLength(1)
})

test('invalid subsequent frames retain the last valid data and set an error state', async () => {
  const { watch, sources } = setup()
  const { observer } = watch<number>(
    ['nodes'],
    '/api/nodes/stream',
    (value) => {
      if (typeof value !== 'number') throw new ApiProtocolError()
      return value
    }
  )
  sources[0].snapshot(1)
  await settled(() => expect(observer.getCurrentResult().data).toBe(1))
  sources[0].snapshot('bad')
  expect(observer.getCurrentResult().error).toBeInstanceOf(ApiProtocolError)
  expect(observer.getCurrentResult().data).toBe(1)
  sources[0].snapshot(2)
  expect(observer.getCurrentResult().isError).toBe(false)
  expect(observer.getCurrentResult().data).toBe(2)
})

test('business errors surface even after the initial read has settled', async () => {
  const { watch, sources } = setup()
  const { observer } = watch()
  sources[0].snapshot([])
  await settled(() => expect(observer.getCurrentResult().isSuccess).toBe(true))
  sources[0].frame('resource-error', { code: 10003, message: '资源已删除' })
  expect(observer.getCurrentResult().error).toBeInstanceOf(ApiBusinessError)
  expect(observer.getCurrentResult().data).toEqual([])
})

test('failed one-shot options release their stream and manual retry remains available', async () => {
  const { watch, sources } = setup()
  const { observer } = watch(
    ['providers', 'options'],
    '/api/providers/dns/options/stream'
  )
  sources[0].frame('resource-error', { code: 10003, message: '选项暂不可用' })
  await settled(() => {
    expect(observer.getCurrentResult().error).toBeInstanceOf(ApiBusinessError)
    expect(sources[0].closed).toBe(true)
  })
  const retried = observer.refetch()
  sources[1].snapshot([{ id: 'recovered-provider' }])
  expect((await retried).data).toEqual([{ id: 'recovered-provider' }])
  expect(sources[1].closed).toBe(true)
})

test('disabled observers do not retain an open stream', async () => {
  const { watch, sources } = setup()
  const { observer } = watch()
  sources[0].snapshot([])
  await Promise.resolve()
  observer.setOptions({ ...observer.options, enabled: false })
  await Promise.resolve()
  expect(sources[0].closed).toBe(true)
})

test('session expiry recovery is shared and late auth completion cannot revive a closed session', async () => {
  let resolve!: (value: object) => void
  const recover = vi.fn(
    () =>
      new Promise<object>((done) => {
        resolve = done
      })
  )
  const { watch, sources, manager, client } = setup(recover)
  watch(['nodes'])
  watch(['tasks'], '/api/tasks/stream')
  sources[0].frame('session-expired', {})
  sources[1].frame('session-expired', {})
  expect(recover).toHaveBeenCalledTimes(1)
  manager.close()
  client.clear()
  resolve({})
  await Promise.resolve()
  await Promise.resolve()
  expect(sources).toHaveLength(2)
  expect(sources.every((source) => source.closed)).toBe(true)
  sources[0].snapshot(['stale'])
  expect(client.getQueryData(['nodes'])).toBeUndefined()
})

const pageTransition = (type: 'pagehide' | 'pageshow', persisted: boolean) =>
  Object.assign(new Event(type), { persisted })

/** Manager over the real reconnecting transport with a fake page. */
function setupPage() {
  const page = new EventTarget()
  const client = new QueryClient({
    defaultOptions: { queries: { retry: false, staleTime: Infinity } },
  })
  const connections: Array<{ url: string; source: Source }> = []
  const manager = new LiveQueryManager(
    client,
    (url) =>
      reconnectingEventSource(url, {
        idleTimeoutMs: null,
        pageLifecycle: page,
        createSource: (path) => {
          const source = new Source()
          connections.push({ url: path, source })
          return source as unknown as EventSource
        },
      }),
    vi.fn(async () => ({})),
    vi.fn(),
    page
  )
  const observe = (
    key: QueryKey,
    read: (signal: AbortSignal) => Promise<unknown>
  ) => {
    const observer = new QueryObserver(client, {
      queryKey: key,
      queryFn: ({ signal }) => read(signal),
    })
    const unsubscribe = observer.subscribe(() => undefined)
    cleanups.push(() => {
      unsubscribe()
      observer.destroy()
    })
    return observer
  }
  cleanups.push(() => {
    manager.dispose()
    client.clear()
  })
  const open = () => connections.filter(({ source }) => !source.closed)
  return { page, client, manager, connections, open, observe }
}

test('the back/forward cache closes every stream and a restore reopens each mounted one once', async () => {
  const { page, client, manager, connections, open, observe } = setupPage()
  const overview = '/api/overview/stream'
  observe(['overview'], (signal) =>
    manager.readEvent(overview, 'snapshot', ['overview'], signal)
  )
  observe(['overview', 'nodes'], (signal) =>
    manager.readEvent(overview, 'nodes', ['overview', 'nodes'], signal)
  )
  observe(['node'], (signal) =>
    manager.read('/api/nodes/a/stream', ['node'], signal)
  )
  await settled(() => expect(connections).toHaveLength(2))
  connections[0].source.snapshot({ value: 1 })
  connections[0].source.frame('nodes', { code: 0, message: 'ok', data: [1] })
  connections[1].source.snapshot({ id: 'a' })
  await settled(() =>
    expect(client.getQueryData(['overview'])).toEqual({ value: 1 })
  )

  page.dispatchEvent(pageTransition('pagehide', true))
  expect(open()).toHaveLength(0)
  page.dispatchEvent(pageTransition('pageshow', true))
  await settled(() => {
    expect(connections).toHaveLength(4)
    expect(open().map(({ url }) => url)).toEqual([
      overview,
      '/api/nodes/a/stream',
    ])
  })
  // The recovery snapshot updates the cache without another connection.
  connections[2].source.snapshot({ value: 2 })
  connections[2].source.frame('nodes', { code: 0, message: 'ok', data: [2] })
  connections[3].source.snapshot({ id: 'a', name: 'restored' })
  await settled(() => {
    expect(client.getQueryData(['overview'])).toEqual({ value: 2 })
    expect(client.getQueryData(['overview', 'nodes'])).toEqual([2])
    expect(client.getQueryData(['node'])).toEqual({ id: 'a', name: 'restored' })
    expect(connections).toHaveLength(4)
  })
})

test('a one-shot read in flight is cancelled on pagehide and read once more on restore', async () => {
  const { page, client, manager, connections, open, observe } = setupPage()
  const url = '/api/clusters/options/stream'
  const options = observe(['clusters', 'options'], (signal) =>
    manager.read(url, ['clusters', 'options'], signal)
  )
  await settled(() => expect(connections).toHaveLength(1))

  page.dispatchEvent(pageTransition('pagehide', true))
  expect(open()).toHaveLength(0)
  await settled(() => {
    // Reverted instead of failed: no error is shown for the cancelled read.
    const result = options.getCurrentResult()
    expect(result.isError).toBe(false)
    expect(result.fetchStatus).toBe('idle')
  })
  page.dispatchEvent(pageTransition('pageshow', true))
  await settled(() => expect(connections).toHaveLength(2))
  connections[1].source.snapshot([{ id: 'cluster-a' }])
  await settled(() => {
    expect(client.getQueryData(['clusters', 'options'])).toEqual([
      { id: 'cluster-a' },
    ])
    expect(open()).toHaveLength(0)
  })
  page.dispatchEvent(pageTransition('pagehide', true))
  page.dispatchEvent(pageTransition('pageshow', true))
  await settled(() => expect(connections).toHaveLength(2))
})

describe('minimum loading time of first loads', () => {
  afterEach(() => {
    vi.useRealTimers()
  })

  // `advanceTimersByTimeAsync` is Vitest-only; `bun test` runs the same suite.
  // Settles query-function promises around the fake clock, then runs the
  // observer notifications they scheduled.
  const advance = async (ms: number) => {
    for (let turn = 0; turn < 5; turn++) await Promise.resolve()
    vi.advanceTimersByTime(ms)
    for (let turn = 0; turn < 5; turn++) await Promise.resolve()
    vi.advanceTimersByTime(0)
    for (let turn = 0; turn < 5; turn++) await Promise.resolve()
  }

  /** Manager with the console's minimum loading time over fake streams. */
  function setupHeld(epoch?: LoadingEpoch) {
    vi.useFakeTimers()
    // No garbage-collection timers: every pending timer is a loading hold.
    const client = new QueryClient({
      defaultOptions: {
        queries: { retry: false, staleTime: Infinity, gcTime: Infinity },
      },
    })
    const sources: Source[] = []
    const manager = new LiveQueryManager(
      client,
      () => {
        const source = new Source()
        sources.push(source)
        return source
      },
      undefined,
      undefined,
      null,
      minimumLoadingMs,
      epoch
    )
    cleanups.push(() => {
      manager.dispose()
      client.clear()
    })
    const watch = (key: QueryKey = ['nodes'], url = '/api/nodes/stream') => {
      const observer = new QueryObserver(client, {
        queryKey: key,
        queryFn: ({ queryKey, signal }) => manager.read(url, queryKey, signal),
      })
      const unsubscribe = observer.subscribe(() => undefined)
      cleanups.push(() => {
        unsubscribe()
        observer.destroy()
      })
      return { observer, unsubscribe }
    }
    return { client, sources, manager, watch }
  }

  test('an early first frame keeps every observer loading for the full minimum', async () => {
    const { sources, watch } = setupHeld()
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
    expect(vi.getTimerCount()).toBe(0)
  })

  test('an early error waits for the minimum too, and a frame in time replaces it', async () => {
    const { sources, watch } = setupHeld()
    const { observer } = watch()
    const failure = { code: 16501, message: '节点不存在', data: null }
    sources[0].frame('resource-error', failure)
    await advance(minimumLoadingMs - 10)
    expect(observer.getCurrentResult().status).toBe('pending')
    await advance(10)
    expect(observer.getCurrentResult().error).toBeInstanceOf(ApiBusinessError)

    const second = watch(['nodes', 'b'], '/api/nodes/b/stream')
    sources[1].frame('error', null)
    sources[1].snapshot({ id: 'b' })
    await advance(minimumLoadingMs)
    expect(second.observer.getCurrentResult().isError).toBe(false)
    expect(second.observer.getCurrentResult().data).toEqual({ id: 'b' })
  })

  test('a revisit with cached data renders at once and never shows loading', async () => {
    const { sources, watch } = setupHeld()
    const first = watch()
    sources[0].snapshot({ value: 1 })
    await advance(minimumLoadingMs)
    first.unsubscribe()
    await advance(0)
    expect(sources[0].closed).toBe(true)

    const again = watch()
    expect(again.observer.getCurrentResult().isPending).toBe(false)
    expect(again.observer.getCurrentResult().data).toEqual({ value: 1 })
    await advance(0)
    // The reopened stream's snapshot applies without a hold.
    sources[1].snapshot({ value: 2 })
    expect(again.observer.getCurrentResult().data).toEqual({ value: 2 })
    expect(vi.getTimerCount()).toBe(0)
  })

  test('a refetch of loaded data never brings the loading state back', async () => {
    const { client, sources, watch } = setupHeld()
    const { observer } = watch()
    sources[0].snapshot({ value: 1 })
    await advance(minimumLoadingMs)
    const states: boolean[] = []
    const stop = observer.subscribe((result) => states.push(result.isPending))
    void client.invalidateQueries({ queryKey: ['nodes'] })
    await advance(0)
    sources.at(-1)?.snapshot({ value: 2 })
    await advance(0)
    stop()
    expect(observer.getCurrentResult().data).toEqual({ value: 2 })
    expect(states.every((pending) => !pending)).toBe(true)
    expect(vi.getTimerCount()).toBe(0)
  })

  test('a page change keeping the previous rows is not held', async () => {
    const { client, sources, manager } = setupHeld()
    const page = (number: number) => ({
      queryKey: ['websites', number],
      queryFn: ({ signal }: { signal: AbortSignal }) =>
        manager.read(
          `/api/websites/stream?page=${number}`,
          ['websites', number],
          signal
        ),
      placeholderData: keepPreviousData,
    })
    const list = new QueryObserver<unknown>(client, page(1))
    cleanups.push(list.subscribe(() => undefined))
    sources[0].snapshot({ page: 1 })
    await advance(minimumLoadingMs)
    expect(list.getCurrentResult().data).toEqual({ page: 1 })

    list.setOptions(page(2))
    expect(list.getCurrentResult().isPlaceholderData).toBe(true)
    await advance(0)
    sources[1].snapshot({ page: 2 })
    await advance(0)
    expect(list.getCurrentResult().data).toEqual({ page: 2 })
    expect(vi.getTimerCount()).toBe(0)
  })

  test('leaving before the minimum passed clears the hold and its timer', async () => {
    const { client, sources, watch } = setupHeld()
    const { unsubscribe } = watch()
    sources[0].snapshot({ value: 1 })
    expect(vi.getTimerCount()).toBe(1)
    unsubscribe()
    await advance(0)
    expect(sources[0].closed).toBe(true)
    expect(vi.getTimerCount()).toBe(0)
    await advance(minimumLoadingMs)
    expect(client.getQueryData(['nodes'])).toBeUndefined()
  })

  test('the hold counts from when the view started loading, across keys it waits on', async () => {
    const { client, sources, manager, watch } = setupHeld()
    const count = (trigger?: string) => ({
      queryKey: ['tasks', 'count', trigger],
      enabled: trigger !== undefined,
      queryFn: ({ signal }: { signal: AbortSignal }) =>
        manager.read(
          `/api/tasks/stream?trigger=${trigger}`,
          ['tasks', 'count', trigger],
          signal
        ),
    })
    // The KPI shows its skeleton from mounting, waiting for the list.
    const kpi = new QueryObserver<unknown>(client, count())
    cleanups.push(kpi.subscribe(() => undefined))
    await advance(300)
    kpi.setOptions(count('list-loaded'))
    await advance(20)
    sources[0].snapshot({ total: 4 })
    await advance(minimumLoadingMs - 330)
    expect(kpi.getCurrentResult().isPending).toBe(true)
    await advance(10)
    expect(kpi.getCurrentResult().data).toEqual({ total: 4 })

    // A view that only now starts loading keeps its own full minimum.
    const table = watch(['nodes', 'tab'], '/api/nodes/tab/stream')
    sources[1].snapshot({ rows: 1 })
    await advance(minimumLoadingMs - 10)
    expect(table.observer.getCurrentResult().isPending).toBe(true)
    await advance(10)
    expect(table.observer.getCurrentResult().data).toEqual({ rows: 1 })
  })

  test('a one-shot read releases its stream at once while its snapshot is held', async () => {
    const { client, sources, watch } = setupHeld()
    const { observer } = watch(
      ['clusters', 'options'],
      '/api/clusters/options/stream'
    )
    sources[0].snapshot([{ id: 'cluster-a' }])
    expect(sources[0].closed).toBe(true)
    await advance(minimumLoadingMs - 10)
    expect(observer.getCurrentResult().isPending).toBe(true)
    await advance(10)
    expect(client.getQueryData(['clusters', 'options'])).toEqual([
      { id: 'cluster-a' },
    ])
    expect(observer.getCurrentResult().isSuccess).toBe(true)
  })

  test('within one navigation a second-step read reveals on arrival; the next navigation is held again', async () => {
    const epoch = new LoadingEpoch()
    const { sources, watch } = setupHeld(epoch)
    // Cluster detail: the header snapshot first, the tab's list after it.
    epoch.begin()
    await advance(30)
    const header = watch(['clusters', 'snapshot'], '/api/clusters/c/stream')
    sources[0].snapshot({ name: 'c' })
    await advance(minimumLoadingMs - 40)
    expect(header.observer.getCurrentResult().isPending).toBe(true)
    await advance(10)
    expect(header.observer.getCurrentResult().data).toEqual({ name: 'c' })
    const list = watch(['nodes', 'cluster', 'c'], '/api/nodes/stream?c')
    await advance(60)
    sources[1].snapshot({ list: [] })
    await advance(0)
    // Its skeleton has been part of the page since the switch: no new hold.
    expect(list.observer.getCurrentResult().data).toEqual({ list: [] })
    expect(vi.getTimerCount()).toBe(0)

    // A later navigation is a new epoch with its own full minimum.
    await advance(1000)
    epoch.begin()
    const next = watch(['websites', 'w'], '/api/websites/w/stream')
    sources[2].snapshot({ id: 'w' })
    await advance(minimumLoadingMs - 10)
    expect(next.observer.getCurrentResult().isPending).toBe(true)
    await advance(10)
    expect(next.observer.getCurrentResult().data).toEqual({ id: 'w' })
  })
})
