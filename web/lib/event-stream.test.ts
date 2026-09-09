import { expect, test, vi } from 'vitest'
import { reconnectingEventSource } from './event-stream'

class Source extends EventTarget {
  readyState = 1
  closed = false
  close() {
    this.closed = true
    this.readyState = 2
  }
  fail(state: number) {
    this.readyState = state
    this.dispatchEvent(new Event('error'))
  }
}

const settle = () => new Promise((resolve) => setTimeout(resolve, 15))

test('cursor streams resume every replacement from the latest delivered id', async () => {
  const sources: Source[] = []
  const urls: string[] = []
  const stream = reconnectingEventSource('/logs?limit=100', {
    retryDelayMs: 1,
    idleTimeoutMs: null,
    resumeQueryParam: 'after',
    createSource: (url) => {
      urls.push(url)
      const source = new Source()
      sources.push(source)
      return source as unknown as EventSource
    },
  })
  let errors = 0
  stream.addEventListener('logs', () => undefined)
  stream.addEventListener('ready', () => undefined)
  stream.addEventListener('error', () => {
    errors++
  })
  try {
    sources[0].dispatchEvent(
      new MessageEvent('logs', { lastEventId: 'cursor:1', data: '{}' })
    )
    sources[0].fail(0)
    expect(sources[0].closed).toBe(true)
    await settle()
    expect(urls[1]).toBe('/logs?limit=100&after=cursor%3A1')
    sources[1].dispatchEvent(new MessageEvent('ready', { data: '{}' }))
    sources[1].fail(2)
    await settle()
    expect(urls[2]).toBe('/logs?limit=100&after=cursor%3A1')
    sources[2].dispatchEvent(
      new MessageEvent('logs', { lastEventId: 'cursor:2', data: '{}' })
    )
    sources[2].fail(0)
    await settle()
    expect(urls[3]).toBe('/logs?limit=100&after=cursor%3A2')
    expect(errors).toBe(3)
  } finally {
    stream.close()
  }
})

test('a stream without observable heartbeat events does not arm an idle watchdog', () => {
  const timer = vi.spyOn(globalThis, 'setTimeout')
  const source = new Source()
  const stream = reconnectingEventSource('/dashboard', {
    idleTimeoutMs: null,
    createSource: () => source as unknown as EventSource,
  })
  try {
    source.dispatchEvent(new Event('open'))
    source.dispatchEvent(new Event('heartbeat'))
    expect(timer).not.toHaveBeenCalled()
  } finally {
    stream.close()
    timer.mockRestore()
  }
})

test('registering the same listener twice delivers each event once', () => {
  const source = new Source()
  const stream = reconnectingEventSource('/events', {
    createSource: () => source as unknown as EventSource,
  })
  let updates = 0
  const listener = () => {
    updates++
  }
  try {
    stream.addEventListener('update', listener)
    stream.addEventListener('update', listener)
    source.dispatchEvent(new Event('update'))
    expect(updates).toBe(1)
  } finally {
    stream.close()
  }
})

test('retired connections cannot deliver events to the active subscription', async () => {
  const sources: Source[] = []
  const stream = reconnectingEventSource('/events', {
    retryDelayMs: 1,
    createSource: () => {
      const source = new Source()
      sources.push(source)
      return source as unknown as EventSource
    },
  })
  let updates = 0
  stream.addEventListener('update', () => {
    updates++
  })
  try {
    sources[0].fail(2)
    await settle()
    expect(sources).toHaveLength(2)
    sources[0].dispatchEvent(new Event('update'))
    expect(updates).toBe(0)
    sources[1].dispatchEvent(new Event('update'))
    expect(updates).toBe(1)
  } finally {
    stream.close()
  }
})

test('closing a subscription detaches listeners and prevents late delivery', () => {
  const source = new Source()
  const stream = reconnectingEventSource('/events', {
    createSource: () => source as unknown as EventSource,
  })
  let updates = 0
  stream.addEventListener('update', () => {
    updates++
  })
  stream.close()
  stream.close()
  stream.addEventListener('update', () => {
    updates++
  })
  source.dispatchEvent(new Event('update'))
  expect(updates).toBe(0)
})

test('a silent dead connection is replaced even without a browser error', async () => {
  const sources: Source[] = []
  const stream = reconnectingEventSource('/events', {
    idleTimeoutMs: 5,
    createSource: () => {
      const source = new Source()
      sources.push(source)
      return source as unknown as EventSource
    },
  })
  await settle()
  expect(sources.length).toBeGreaterThan(1)
  expect(sources[0].closed).toBe(true)
  stream.close()
})

test('terminal HTTP failure reconnects and preserves ready/state handlers', async () => {
  const sources: Source[] = []
  const stream = reconnectingEventSource('/events', {
    retryDelayMs: 1,
    createSource: () => {
      const source = new Source()
      sources.push(source)
      return source as unknown as EventSource
    },
  })
  let ready = 0
  let updates = 0
  stream.addEventListener('ready', () => {
    ready++
  })
  stream.addEventListener('task-state', () => {
    updates++
  })
  sources[0].fail(2)
  sources[0].fail(2)
  await settle()
  expect(sources).toHaveLength(2)
  expect(sources[0].closed).toBe(true)
  sources[1].dispatchEvent(new Event('ready'))
  sources[1].dispatchEvent(new Event('task-state'))
  expect([ready, updates]).toEqual([1, 1])
  stream.close()
})

test('native reconnect is not duplicated and unmount cancels terminal retry', async () => {
  const sources: Source[] = []
  const stream = reconnectingEventSource('/events', {
    retryDelayMs: 1,
    createSource: () => {
      const source = new Source()
      sources.push(source)
      return source as unknown as EventSource
    },
  })
  sources[0].fail(0)
  await settle()
  expect(sources).toHaveLength(1)
  sources[0].fail(2)
  stream.close()
  await settle()
  expect(sources).toHaveLength(1)
  expect(sources[0].closed).toBe(true)
})
