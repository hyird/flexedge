import { afterEach, describe, expect, test, vi } from 'vitest'
import {
  createEventStreamParser,
  openEventStream,
  type ServerSentEvent,
} from './event-stream'

function parse(chunks: string[], lastEventId = '') {
  const events: ServerSentEvent[] = []
  const retries: number[] = []
  const parser = createEventStreamParser(
    {
      event: (event) => events.push(event),
      retry: (delayMs) => retries.push(delayMs),
    },
    lastEventId
  )
  for (const chunk of chunks) parser.push(chunk)
  return { events, retries, lastEventId: parser.lastEventId }
}

describe('event stream parser', () => {
  test('dispatches named events and joins multi-line data with LF', () => {
    expect(
      parse(['event: logs\ndata: {"a":1}\ndata:second\nid: 7\n\n']).events
    ).toEqual([{ type: 'logs', data: '{"a":1}\nsecond', lastEventId: '7' }])
  })

  test('accepts CRLF, LF and CR line endings, also split across chunks', () => {
    const { events } = parse([
      'data: one\r',
      '\n\r\ndata: two\r\rdata: three\n',
      '\n',
    ])
    expect(events.map((event) => event.data)).toEqual(['one', 'two', 'three'])
    expect(events.every((event) => event.type === 'message')).toBe(true)
  })

  test('keeps a field line split inside a chunk', () => {
    expect(parse(['da', 'ta: sp', 'lit\n', '\n']).events[0].data).toBe('split')
  })

  test('ignores comments, unknown fields and blank keep-alive frames', () => {
    const { events } = parse([': keep-alive\n\n', '\n', 'foo: bar\n\n'])
    expect(events).toEqual([])
  })

  test('an id persists across events, survives id-only blocks and rejects NUL', () => {
    const result = parse(
      ['data: a\n\n', 'id: 9\n\n', 'data: b\n\n', 'id: bad\0id\ndata: c\n\n'],
      '3'
    )
    expect(result.events.map((event) => event.lastEventId)).toEqual([
      '3',
      '9',
      '9',
    ])
    expect(result.lastEventId).toBe('9')
  })

  test('reports numeric retry fields only and discards an unterminated block', () => {
    const { events, retries } = parse(['retry: 3000\nretry: soon\n', 'data: x'])
    expect(retries).toEqual([3000])
    expect(events).toEqual([])
  })

  test('a field without a colon has an empty value', () => {
    expect(parse(['data\ndata\n\n']).events[0].data).toBe('\n')
  })
})

/** A scripted `fetch`: every request takes the next queued response. */
function scriptedFetch() {
  const requests: Array<{ url: string; headers: Record<string, string> }> = []
  const bodies: Array<ReadableStreamDefaultController<Uint8Array>> = []
  const responses: Array<() => Response> = []
  let cancelled = 0
  const encoder = new TextEncoder()
  const fetch = vi.fn(async (url: string, init: RequestInit) => {
    requests.push({ url, headers: init.headers as Record<string, string> })
    const next = responses.shift()
    if (!next) throw new TypeError('network down')
    return next()
  })
  return {
    fetch,
    requests,
    /** Queues an accepted event stream; `send` writes to the latest one. */
    accept() {
      responses.push(
        () =>
          new Response(
            new ReadableStream<Uint8Array>({
              start: (controller) => {
                bodies.push(controller)
              },
              cancel: () => {
                cancelled++
              },
            }),
            { headers: { 'content-type': 'text/event-stream' } }
          )
      )
    },
    refuse(status: number) {
      responses.push(() => new Response('{}', { status }))
    },
    send(text: string) {
      bodies.at(-1)?.enqueue(encoder.encode(text))
    },
    end() {
      bodies.at(-1)?.close()
    },
    /** Accepted bodies the client released (cancelled). */
    cancelled: () => cancelled,
  }
}

async function flush() {
  for (let turn = 0; turn < 20; turn++) await Promise.resolve()
}

describe('event stream connection', () => {
  afterEach(() => {
    vi.useRealTimers()
  })

  test('delivers events, keeps the cursor and resumes with Last-Event-ID', async () => {
    vi.useFakeTimers()
    const server = scriptedFetch()
    server.accept()
    server.accept()
    const stream = openEventStream('/api/nodes/n/logs/stream?limit=100', {
      fetch: server.fetch,
      pageLifecycle: null,
    })
    const received: string[] = []
    stream.addEventListener('logs', (event) =>
      received.push((event as MessageEvent<string>).data)
    )
    await flush()
    server.send('retry: 1000\nevent: logs\nid: c1\ndata: first\n\n')
    await flush()
    server.end()
    await flush()
    vi.advanceTimersByTime(1000)
    await flush()
    server.send('event: logs\nid: c2\ndata: second\n\n')
    await flush()
    expect(received).toEqual(['first', 'second'])
    expect(server.requests.map((request) => request.headers)).toEqual([
      { Accept: 'text/event-stream' },
      { Accept: 'text/event-stream', 'Last-Event-ID': 'c1' },
    ])
    stream.close()
  })

  test('keep-alive frames renew the idle timer; silence replaces the connection', async () => {
    vi.useFakeTimers()
    const server = scriptedFetch()
    server.accept()
    server.accept()
    const stream = openEventStream('/events', {
      fetch: server.fetch,
      idleTimeoutMs: 1000,
      retryDelayMs: 10,
      pageLifecycle: null,
    })
    const errors = vi.fn()
    const messages = vi.fn()
    stream.addEventListener('error', errors)
    stream.addEventListener('message', messages)
    await flush()
    for (let beat = 0; beat < 3; beat++) {
      vi.advanceTimersByTime(800)
      server.send('\n')
      await flush()
    }
    expect(errors).not.toHaveBeenCalled()
    expect(messages).not.toHaveBeenCalled()
    expect(server.fetch).toHaveBeenCalledTimes(1)
    vi.advanceTimersByTime(1000)
    await flush()
    expect(errors).toHaveBeenCalledTimes(1)
    vi.advanceTimersByTime(10)
    await flush()
    expect(server.fetch).toHaveBeenCalledTimes(2)
    stream.close()
  })

  test('refused handshakes back off and give up; an accepted one resets the budget', async () => {
    vi.useFakeTimers()
    const server = scriptedFetch()
    server.refuse(502)
    server.accept()
    for (let attempt = 0; attempt < 3; attempt++) server.refuse(404)
    const stream = openEventStream('/missing', {
      fetch: server.fetch,
      retryDelayMs: 100,
      maxRetryDelayMs: 250,
      maxRejectedAttempts: 3,
      pageLifecycle: null,
    })
    const exhausted = vi.fn()
    stream.addEventListener('exhausted', exhausted)
    await flush()
    vi.advanceTimersByTime(100)
    await flush()
    expect(server.fetch).toHaveBeenCalledTimes(2)
    server.end()
    await flush()
    // The accepted handshake reset the backoff: 100, 200, then the cap.
    for (const delay of [100, 200, 250]) {
      vi.advanceTimersByTime(delay - 1)
      await flush()
      const before = server.fetch.mock.calls.length
      vi.advanceTimersByTime(1)
      await flush()
      expect(server.fetch.mock.calls.length).toBe(before + 1)
    }
    expect(exhausted).toHaveBeenCalledTimes(1)
    vi.advanceTimersByTime(10_000)
    await flush()
    expect(server.fetch).toHaveBeenCalledTimes(5)
  })

  test('a 401 handshake or a session-expired frame ends the stream', async () => {
    const server = scriptedFetch()
    server.refuse(401)
    const stream = openEventStream('/events', {
      fetch: server.fetch,
      pageLifecycle: null,
    })
    const expired = vi.fn()
    stream.addEventListener('session-expired', expired)
    await flush()
    expect(expired).toHaveBeenCalledTimes(1)

    server.accept()
    const second = openEventStream('/events', {
      fetch: server.fetch,
      pageLifecycle: null,
    })
    const frames = vi.fn()
    second.addEventListener('session-expired', frames)
    second.addEventListener('error', frames)
    await flush()
    server.send('event: session-expired\ndata: {}\n\n')
    await flush()
    // The stream stopped: it released the body itself and never reconnects.
    expect(server.cancelled()).toBe(1)
    expect(frames).toHaveBeenCalledTimes(1)
    expect(server.fetch).toHaveBeenCalledTimes(2)
  })

  test('closing stops delivery and pending reconnects', async () => {
    vi.useFakeTimers()
    const server = scriptedFetch()
    server.accept()
    const stream = openEventStream('/events', {
      fetch: server.fetch,
      pageLifecycle: null,
    })
    const messages = vi.fn()
    stream.addEventListener('message', messages)
    await flush()
    stream.close()
    server.send('data: late\n\n')
    await flush()
    vi.advanceTimersByTime(60_000)
    await flush()
    expect(messages).not.toHaveBeenCalled()
    expect(server.fetch).toHaveBeenCalledTimes(1)
  })

  test('the back/forward cache closes the connection and a restore reopens it once', async () => {
    const server = scriptedFetch()
    server.accept()
    server.accept()
    const page = new EventTarget()
    const stream = openEventStream('/events', {
      fetch: server.fetch,
      pageLifecycle: page,
    })
    const messages = vi.fn()
    stream.addEventListener('message', messages)
    await flush()
    page.dispatchEvent(new Event('pagehide'))
    server.send('data: hidden\n\n')
    await flush()
    // A fresh pageshow (not from the cache) is no restore.
    page.dispatchEvent(new Event('pageshow'))
    await flush()
    expect(server.fetch).toHaveBeenCalledTimes(1)
    page.dispatchEvent(
      Object.assign(new Event('pageshow'), { persisted: true })
    )
    await flush()
    server.send('data: restored\n\n')
    await flush()
    expect(server.fetch).toHaveBeenCalledTimes(2)
    expect(messages.mock.calls.map(([event]) => event.data)).toEqual([
      'restored',
    ])
    stream.close()
  })
})
