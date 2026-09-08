import { expect, test } from 'vitest'
import { reconnectingEventSource } from './event-stream'

class Source extends EventTarget {
  readyState = 1
  closed = false
  close() {
    this.closed = true
  }
  fail(state: number) {
    this.readyState = state
    this.dispatchEvent(new Event('error'))
  }
}

const settle = () => new Promise((resolve) => setTimeout(resolve, 15))

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
