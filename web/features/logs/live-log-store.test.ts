import { z } from 'zod'
import { describe, expect, it } from 'vitest'
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
