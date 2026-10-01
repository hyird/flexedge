import {
  QueryClient,
  QueryObserver,
  type QueryKey,
} from '@tanstack/react-query'
import { afterEach, expect, test, vi } from 'vitest'
import { ApiBusinessError, ApiProtocolError } from './api-response'
import { LiveQueryManager } from './live-query'

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
