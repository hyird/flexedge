import { z } from 'zod'
import { afterEach, describe, expect, it, vi } from 'vitest'
import { minimumLoadingMs } from '@/lib/minimum-loading'
import {
  createLiveLogStore,
  liveLogCapacity,
  mergeLiveLogs,
  type LogEntry,
} from './live-log-store'
import { createLogEventParser } from './log-event'

const parseLogs = createLogEventParser(
  z.object({ id: z.string(), occurred_at: z.string() })
)

class Source extends EventTarget {
  closes = 0
  close() {
    this.closes++
  }
  logs(list: LogEntry[]) {
    this.dispatchEvent(
      new MessageEvent('logs', {
        data: JSON.stringify({ code: 0, message: '', data: { list } }),
      })
    )
  }
}
const log = (id: string, occurred_at = id) => ({ id, occurred_at })

describe('live log buffers', () => {
  it('manual reconnect closes the old source before opening a new one and retains records', () => {
    const sources: Source[] = []
    const store = createLiveLogStore('/node/a', parseLogs, () => {
      expect(sources.filter((source) => source.closes === 0)).toHaveLength(0)
      const source = new Source()
      sources.push(source)
      return source
    })
    store.reconnect()
    expect(sources).toHaveLength(0)
    const stop = store.subscribe(() => undefined)
    sources[0].logs([log('one')])
    store.reconnect()
    expect(sources[0].closes).toBe(1)
    expect(sources).toHaveLength(2)
    sources[0].logs([log('retired')])
    expect(store.getSnapshot().logs).toEqual([log('one')])
    sources[1].logs([log('two')])
    expect(store.getSnapshot().logs.map((entry) => entry.id)).toEqual([
      'two',
      'one',
    ])
    stop()
    expect(sources[1].closes).toBe(1)
  })
  it('preserves valid records on parse failure and clears the error after a valid batch', () => {
    const source = new Source()
    const store = createLiveLogStore('/node/a', parseLogs, () => source)
    const stop = store.subscribe(() => undefined)
    try {
      source.logs([log('one')])
      source.dispatchEvent(new MessageEvent('logs', { data: '{' }))
      expect(store.getSnapshot().logs).toEqual([log('one')])
      expect(store.getSnapshot().error).toContain('格式不正确')
      source.logs([log('two')])
      expect(store.getSnapshot().error).toBeNull()
      expect(store.getSnapshot().logs.map((entry) => entry.id)).toEqual([
        'two',
        'one',
      ])
    } finally {
      stop()
    }
  })
  it('uses the latest version of each record, sorts by time and bounds retained data', () => {
    const current = Array.from({ length: liveLogCapacity }, (_, index) =>
      log(String(index), String(index).padStart(5, '0'))
    )
    const incoming = [log('0', '99999'), log('new', '99998')]
    const merged = mergeLiveLogs(current, incoming)
    expect(merged).toHaveLength(liveLogCapacity)
    expect(merged.slice(0, 2)).toEqual(incoming)
    expect(merged.find((entry) => entry.id === '1')).toBeUndefined()
    expect(current[0].occurred_at).toBe('00000')
  })

  it('opens one connection for observers and closes it after the last subscription', () => {
    const sources: Source[] = []
    const store = createLiveLogStore('/node/a', parseLogs, () => {
      const source = new Source()
      sources.push(source)
      return source
    })
    let changes = 0
    const listener = () => {
      changes++
    }
    expect(sources).toHaveLength(0)
    const first = store.subscribe(listener)
    const second = store.subscribe(listener)
    sources[0].dispatchEvent(new Event('ready'))
    expect(store.getSnapshot().connected).toBe(true)
    expect(changes).toBe(2)
    first()
    first()
    expect(sources[0].closes).toBe(0)
    second()
    expect(sources[0].closes).toBe(1)
    expect(store.getSnapshot().connected).toBe(false)
  })

  it('loads until the first batch even after ready, and stops loading when the connection fails', () => {
    const sources: Source[] = []
    const store = createLiveLogStore('/website/a', parseLogs, () => {
      const source = new Source()
      sources.push(source)
      return source
    })
    const stop = store.subscribe(() => undefined)
    try {
      expect(store.getSnapshot().loading).toBe(true)
      sources[0].dispatchEvent(new Event('ready'))
      expect(store.getSnapshot().loading).toBe(true)
      // An empty first batch is still the first batch.
      sources[0].logs([])
      expect(store.getSnapshot().loading).toBe(false)
    } finally {
      stop()
    }

    const failing = createLiveLogStore('/website/b', parseLogs, () => {
      const source = new Source()
      sources.push(source)
      return source
    })
    const stopFailing = failing.subscribe(() => undefined)
    try {
      sources[1].dispatchEvent(new Event('error'))
      expect(failing.getSnapshot().loading).toBe(false)
    } finally {
      stopFailing()
    }
  })

  it('preserves the resource buffer across pause and ignores retired connection data', () => {
    const sources: Source[] = []
    const store = createLiveLogStore('/website/a', parseLogs, () => {
      const source = new Source()
      sources.push(source)
      return source
    })
    const stop = store.subscribe(() => undefined)
    sources[0].logs([log('one')])
    stop()
    const resume = store.subscribe(() => undefined)
    try {
      sources[0].logs([log('old-connection')])
      sources[1].logs([log('two')])
      expect(store.getSnapshot().logs.map((entry) => entry.id)).toEqual([
        'two',
        'one',
      ])
      sources[1].dispatchEvent(new Event('ready'))
      sources[1].dispatchEvent(new Event('error'))
      expect(store.getSnapshot().connected).toBe(false)
    } finally {
      resume()
    }
  })

  it('keeps different resource URLs isolated', () => {
    const sourceA = new Source(),
      sourceB = new Source()
    const first = createLiveLogStore('/node/a', parseLogs, () => sourceA)
    const second = createLiveLogStore('/node/b', parseLogs, () => sourceB)
    const stopA = first.subscribe(() => undefined),
      stopB = second.subscribe(() => undefined)
    try {
      sourceA.logs([log('same-id', 'a')])
      sourceB.logs([log('same-id', 'b')])
      expect(first.getSnapshot().logs).toEqual([log('same-id', 'a')])
      expect(second.getSnapshot().logs).toEqual([log('same-id', 'b')])
    } finally {
      stopA()
      stopB()
    }
  })
})

describe('minimum loading time of the first batch', () => {
  afterEach(() => {
    vi.useRealTimers()
  })
  const heldStore = (source: Source) =>
    createLiveLogStore(
      '/node/a',
      parseLogs,
      () => source,
      undefined,
      minimumLoadingMs
    )

  it('keeps the tail loading for the full minimum, then publishes every batch at once', () => {
    vi.useFakeTimers()
    const source = new Source()
    const store = heldStore(source)
    let renders = 0
    const stop = store.subscribe(() => renders++)
    try {
      vi.advanceTimersByTime(40)
      source.logs([log('one')])
      source.logs([log('two')])
      vi.advanceTimersByTime(minimumLoadingMs - 41)
      expect(store.getSnapshot()).toMatchObject({ loading: true, logs: [] })
      expect(renders).toBe(0)
      vi.advanceTimersByTime(1)
      expect(store.getSnapshot().loading).toBe(false)
      expect(store.getSnapshot().logs.map((entry) => entry.id)).toEqual([
        'two',
        'one',
      ])
      expect(renders).toBe(1)
      // Later batches and reconnects never bring the loading state back.
      source.logs([log('three')])
      expect(store.getSnapshot().logs).toHaveLength(3)
      store.reconnect()
      expect(store.getSnapshot().loading).toBe(false)
      expect(vi.getTimerCount()).toBe(0)
    } finally {
      stop()
    }
  })

  it('drops a held first batch and its timer when the tail closes', () => {
    vi.useFakeTimers()
    const source = new Source()
    const store = heldStore(source)
    const stop = store.subscribe(() => undefined)
    source.logs([log('one')])
    expect(vi.getTimerCount()).toBe(1)
    stop()
    expect(vi.getTimerCount()).toBe(0)
    expect(store.getSnapshot()).toMatchObject({ loading: true, logs: [] })
  })
})
