/** One dispatched server-sent event. */
export type ServerSentEvent = {
  type: string
  data: string
  lastEventId: string
}

/** Incremental `text/event-stream` parser (WHATWG HTML, "Parsing an event
 * stream"). Lines end with CRLF, LF or CR, also across chunk boundaries.
 * Comment lines (`:`) and blank lines without data dispatch nothing; `data`
 * lines join with LF; `id` (without NUL) sets the last event ID even when no
 * event follows; a numeric `retry` reports the reconnection time. A block
 * still open when the stream ends is discarded by not calling `push` again.
 * The caller decodes UTF-8 (`TextDecoder` strips the BOM). */
export function createEventStreamParser(
  handlers: {
    event: (event: ServerSentEvent) => void
    retry?: (delayMs: number) => void
  },
  lastEventId = ''
) {
  let buffer = ''
  // A chunk ended with CR: a LF opening the next chunk ends the same line.
  let skipLineFeed = false
  let data = ''
  let type = ''
  let idBuffer = lastEventId

  const dispatch = () => {
    lastEventId = idBuffer
    if (data === '') {
      type = ''
      return
    }
    const event = {
      type: type || 'message',
      data: data.endsWith('\n') ? data.slice(0, -1) : data,
      lastEventId,
    }
    data = ''
    type = ''
    handlers.event(event)
  }

  const line = (text: string) => {
    if (text === '') {
      dispatch()
      return
    }
    if (text.startsWith(':')) return
    const colon = text.indexOf(':')
    const field = colon < 0 ? text : text.slice(0, colon)
    let value = colon < 0 ? '' : text.slice(colon + 1)
    if (value.startsWith(' ')) value = value.slice(1)
    if (field === 'event') type = value
    else if (field === 'data') data += value + '\n'
    else if (field === 'id') {
      if (!value.includes('\0')) idBuffer = value
    } else if (field === 'retry' && /^\d+$/.test(value))
      handlers.retry?.(Number(value))
  }

  return {
    push(chunk: string) {
      let text = chunk
      if (skipLineFeed && text) {
        skipLineFeed = false
        if (text.startsWith('\n')) text = text.slice(1)
      }
      buffer += text
      let start = 0
      for (let index = 0; index < buffer.length; index++) {
        const code = buffer.charCodeAt(index)
        if (code !== 10 && code !== 13) continue
        line(buffer.slice(start, index))
        if (code === 13) {
          if (index + 1 === buffer.length) skipLineFeed = true
          else if (buffer.charCodeAt(index + 1) === 10) index++
        }
        start = index + 1
      }
      buffer = buffer.slice(start)
    },
    /** The last event ID: sent as `Last-Event-ID` when reconnecting. */
    get lastEventId() {
      return lastEventId
    },
  }
}

export type EventStreamOptions = {
  fetch?: (input: string, init: RequestInit) => Promise<Response>
  /** First reconnect delay, doubled per consecutive failed attempt; a
   * `retry` field from the server replaces it. */
  retryDelayMs?: number
  /** Upper bound of the reconnect backoff. */
  maxRetryDelayMs?: number
  /** Consecutive refused handshakes (HTTP error) before the stream gives up
   * and emits `exhausted`; an accepted handshake resets it. */
  maxRejectedAttempts?: number
  /** Silence after which the connection is treated as dead and replaced.
   * Every received byte renews it, keep-alive frames included. */
  idleTimeoutMs?: number
  /** Target of `pagehide` / `pageshow` (the window; null outside a browser). */
  pageLifecycle?: EventTarget | null
}

/** Event dispatched once when refused handshakes exhausted the retries. */
export const streamExhaustedEvent = 'exhausted'
/** Event dispatched when the server ends the session (`session-expired`
 * frame or a 401 handshake); the stream stops. */
export const streamSessionExpiredEvent = 'session-expired'

export type EventStream = {
  addEventListener(type: string, handler: EventListener): void
  close(): void
}

/** A server-sent event stream over `fetch`. Every server event is dispatched
 * as a `MessageEvent` of its type; `open` follows an accepted handshake and
 * `error` each interruption, after which the stream reconnects with a
 * bounded exponential backoff and sends the last event ID as
 * `Last-Event-ID`. Refused handshakes (for example a proxy's 500 during
 * deployment, or an endpoint this server does not provide) are abandoned
 * after a few consecutive attempts with `exhausted`, so a missing endpoint
 * never turns into a reconnect loop; the caller offers a manual retry.
 *
 * A proxy can also leave a dead connection open: the server's keep-alive
 * frames bound the silence, and a connection silent for `idleTimeoutMs` is
 * replaced; the reconnect reconciles data without HTTP polling.
 *
 * A page entering the back/forward cache must not keep its connection: on
 * `pagehide` the connection closes and pending retries stop; when the page is
 * restored (`pageshow` with `persisted`) an open stream reconnects once, so
 * the server sends one recovery snapshot (or resumes from the last event ID).
 * Closed streams stay closed. */
export function openEventStream(
  url: string,
  options: EventStreamOptions = {}
): EventStream {
  const request = options.fetch ?? ((input, init) => fetch(input, init))
  const target = new EventTarget()
  const pageLifecycle =
    options.pageLifecycle === undefined
      ? typeof window === 'undefined'
        ? null
        : window
      : options.pageLifecycle
  let connection: AbortController | undefined
  let stopped = false
  // Closed for the back/forward cache; reconnects when the page is restored.
  let suspended = false
  let timer: ReturnType<typeof setTimeout> | undefined
  let idleTimer: ReturnType<typeof setTimeout> | undefined
  let lastEventId = ''
  let retryDelayMs = options.retryDelayMs ?? 3000
  // Consecutive attempts without an accepted handshake.
  let failures = 0
  let rejected = 0

  const emit = (event: Event) => {
    if (!stopped) target.dispatchEvent(event)
  }
  const retire = () => {
    clearTimeout(idleTimer)
    idleTimer = undefined
    connection?.abort()
    connection = undefined
  }
  const stop = () => {
    stopped = true
    clearTimeout(timer)
    timer = undefined
    retire()
    pageLifecycle?.removeEventListener('pagehide', suspend)
    pageLifecycle?.removeEventListener('pageshow', resume)
  }
  const interrupt = (refused: boolean) => {
    retire()
    emit(new Event('error'))
    if (stopped || suspended) return
    if (refused && ++rejected >= (options.maxRejectedAttempts ?? 4)) {
      emit(new Event(streamExhaustedEvent))
      stop()
      return
    }
    const delay = Math.min(
      retryDelayMs * 2 ** failures,
      options.maxRetryDelayMs ?? 30000
    )
    failures++
    timer = setTimeout(() => {
      timer = undefined
      void connect()
    }, delay)
  }
  const activity = (attempt: AbortController) => {
    clearTimeout(idleTimer)
    idleTimer = setTimeout(() => {
      if (connection === attempt) interrupt(false)
    }, options.idleTimeoutMs ?? 45000)
  }
  const connect = async () => {
    const attempt = new AbortController()
    connection = attempt
    activity(attempt)
    const headers: Record<string, string> = { Accept: 'text/event-stream' }
    if (lastEventId) headers['Last-Event-ID'] = lastEventId
    let response: Response
    try {
      response = await request(url, {
        headers,
        credentials: 'include',
        cache: 'no-store',
        signal: attempt.signal,
      })
    } catch {
      if (connection === attempt) interrupt(false)
      return
    }
    if (connection !== attempt) {
      void response.body?.cancel().catch(() => undefined)
      return
    }
    if (response.status === 401) {
      void response.body?.cancel().catch(() => undefined)
      emit(new Event(streamSessionExpiredEvent))
      stop()
      return
    }
    const eventStream =
      response.headers
        .get('content-type')
        ?.toLowerCase()
        .startsWith('text/event-stream') ?? false
    if (!response.ok || !response.body || !eventStream) {
      void response.body?.cancel().catch(() => undefined)
      interrupt(true)
      return
    }
    failures = 0
    rejected = 0
    activity(attempt)
    emit(new Event('open'))
    const parser = createEventStreamParser(
      {
        event: (event) => {
          if (connection !== attempt) return
          lastEventId = event.lastEventId
          emit(
            new MessageEvent(event.type, {
              data: event.data,
              lastEventId: event.lastEventId,
            })
          )
          if (event.type === streamSessionExpiredEvent) stop()
        },
        retry: (delayMs) => {
          retryDelayMs = delayMs
        },
      },
      lastEventId
    )
    const reader = response.body.getReader()
    const decoder = new TextDecoder()
    try {
      while (connection === attempt) {
        const { done, value } = await reader.read()
        if (connection !== attempt) break
        if (done) {
          interrupt(false)
          break
        }
        activity(attempt)
        parser.push(decoder.decode(value, { stream: true }))
        if (connection === attempt) lastEventId = parser.lastEventId
      }
    } catch {
      if (connection === attempt) interrupt(false)
    } finally {
      if (connection !== attempt) void reader.cancel().catch(() => undefined)
    }
  }
  function suspend() {
    if (stopped || suspended) return
    suspended = true
    clearTimeout(timer)
    timer = undefined
    retire()
  }
  function resume(event: Event) {
    if (stopped || !suspended || !(event as PageTransitionEvent).persisted)
      return
    suspended = false
    void connect()
  }

  void connect()
  pageLifecycle?.addEventListener('pagehide', suspend)
  pageLifecycle?.addEventListener('pageshow', resume)
  return {
    addEventListener(type, handler) {
      if (!stopped) target.addEventListener(type, handler)
    },
    close() {
      if (!stopped) stop()
    },
  }
}
