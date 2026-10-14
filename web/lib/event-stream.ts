type Options = {
  createSource?: (url: string) => EventSource
  /** First reconnect delay; doubles per consecutive failed attempt. */
  retryDelayMs?: number
  /** Upper bound of the reconnect backoff. */
  maxRetryDelayMs?: number
  /** Consecutive refused handshakes (HTTP error, readyState CLOSED) before
   * the stream gives up and emits `exhausted`; a successful open resets it. */
  maxRejectedAttempts?: number
  // null for endpoints whose heartbeat frames are not dispatched as DOM events.
  idleTimeoutMs?: number | null
  // Cursor streams replace connections themselves so a fixed query cursor never
  // overrides a newer Last-Event-ID header during a native retry.
  resumeQueryParam?: string
  /** Target of `pagehide` / `pageshow` (the window; null outside a browser). */
  pageLifecycle?: EventTarget | null
}

/** Event dispatched once when refused handshakes exhausted the retries. */
export const streamExhaustedEvent = 'exhausted'

/** Native EventSource retries interrupted streams, but terminal HTTP errors
 * (for example a proxy's 500 during deployment, or an endpoint this server
 * does not provide) leave it CLOSED. Those handshakes are retried with a
 * bounded exponential backoff and abandoned after a few consecutive refusals,
 * so a missing endpoint never turns into a reconnect loop; the caller shows
 * the failure and offers a manual retry. A proxy can also leave a dead
 * connection open. Heartbeats bound that silence; reconnecting ready
 * reconciles current data without HTTP polling.
 *
 * A page entering the back/forward cache must not keep its connection: on
 * `pagehide` the connection closes and pending retries stop; when the page is
 * restored (`pageshow` with `persisted`), an open stream reconnects once
 * through the same path as a replacement, so the server sends one recovery
 * snapshot (or resumes from the cursor). Closed streams stay closed.
 */
export function reconnectingEventSource(url: string, options: Options = {}) {
  const createSource =
    options.createSource ??
    ((path: string) => new EventSource(path, { withCredentials: true }))
  const listeners = new Map<string, Set<EventListener>>()
  let source: EventSource | undefined
  let detachListeners: Array<() => void> = []
  let stopped = false
  let timer: ReturnType<typeof setTimeout> | undefined
  let idleTimer: ReturnType<typeof setTimeout> | undefined
  let lastEventId = ''
  // Consecutive replacement attempts without an opened connection.
  let failures = 0
  let rejected = 0
  // Closed for the back/forward cache; reconnects when the page is restored.
  let suspended = false
  const pageLifecycle =
    options.pageLifecycle === undefined
      ? typeof window === 'undefined'
        ? null
        : window
      : options.pageLifecycle
  const connectionUrl = () => {
    if (!options.resumeQueryParam || !lastEventId) return url
    const hashIndex = url.indexOf('#')
    const base = hashIndex < 0 ? url : url.slice(0, hashIndex)
    const hash = hashIndex < 0 ? '' : url.slice(hashIndex)
    const queryIndex = base.indexOf('?')
    const path = queryIndex < 0 ? base : base.slice(0, queryIndex)
    const query = new URLSearchParams(
      queryIndex < 0 ? '' : base.slice(queryIndex + 1)
    )
    query.set(options.resumeQueryParam, lastEventId)
    return `${path}?${query}${hash}`
  }
  const retireSource = () => {
    const retired = source
    source = undefined
    for (const detach of detachListeners) detach()
    detachListeners = []
    retired?.close()
  }
  const reconnect = () => {
    if (stopped || suspended) return
    if (timer !== undefined) clearTimeout(timer)
    timer = undefined
    retireSource()
    connect()
  }
  const stop = () => {
    stopped = true
    clearTimeout(timer)
    clearTimeout(idleTimer)
    pageLifecycle?.removeEventListener('pagehide', suspend)
    pageLifecycle?.removeEventListener('pageshow', resume)
    retireSource()
  }
  const exhaust = () => {
    stop()
    const handlers = [...(listeners.get(streamExhaustedEvent) ?? [])]
    listeners.clear()
    for (const handler of handlers) handler(new Event(streamExhaustedEvent))
  }
  const activity = () => {
    if (idleTimer !== undefined) clearTimeout(idleTimer)
    if (!stopped && !suspended && options.idleTimeoutMs !== null)
      idleTimer = setTimeout(reconnect, options.idleTimeoutMs ?? 45000)
  }
  function suspend() {
    if (stopped || suspended) return
    suspended = true
    clearTimeout(timer)
    clearTimeout(idleTimer)
    timer = idleTimer = undefined
    retireSource()
  }
  function resume(event: Event) {
    if (stopped || !suspended || !(event as PageTransitionEvent).persisted)
      return
    suspended = false
    connect()
  }
  const attach = (
    connection: EventSource,
    type: string,
    handler: EventListener
  ) => {
    const listener: EventListener = (event) => {
      if (stopped || source !== connection) return
      if (event.type !== 'error' && connection.readyState === 2) return
      if (
        options.resumeQueryParam &&
        event instanceof MessageEvent &&
        event.lastEventId
      ) {
        lastEventId = event.lastEventId
      }
      activity()
      handler(event)
      if (event.type === 'session-expired') {
        stop()
        listeners.clear()
      }
    }
    connection.addEventListener(type, listener)
    detachListeners.push(() => connection.removeEventListener(type, listener))
  }
  const connect = () => {
    const connection = createSource(connectionUrl())
    source = connection
    activity()
    attach(connection, 'open', () => {
      failures = 0
      rejected = 0
    })
    attach(connection, 'heartbeat', () => undefined)
    for (const [type, handlers] of listeners) {
      for (const handler of handlers) attach(connection, type, handler)
    }
    const onError = () => {
      // Snapshot streams retain native retries; cursor streams use their latest
      // delivered id for every replacement, including transport interruption.
      const refused = connection.readyState === 2
      if (
        stopped ||
        source !== connection ||
        (!options.resumeQueryParam && !refused) ||
        timer !== undefined
      )
        return
      if (options.resumeQueryParam) connection.close()
      if (refused && ++rejected >= (options.maxRejectedAttempts ?? 4)) {
        exhaust()
        return
      }
      const delay = Math.min(
        (options.retryDelayMs ?? 3000) * 2 ** failures,
        options.maxRetryDelayMs ?? 30000
      )
      failures++
      timer = setTimeout(reconnect, delay)
    }
    connection.addEventListener('error', onError)
    detachListeners.push(() => connection.removeEventListener('error', onError))
  }
  connect()
  pageLifecycle?.addEventListener('pagehide', suspend)
  pageLifecycle?.addEventListener('pageshow', resume)
  return {
    addEventListener(type: string, handler: EventListener) {
      if (stopped) return
      const handlers = listeners.get(type) ?? new Set<EventListener>()
      if (handlers.has(handler)) return
      handlers.add(handler)
      listeners.set(type, handlers)
      // While suspended the next connection attaches every listener.
      if (source) attach(source, type, handler)
    },
    close() {
      if (stopped) return
      stop()
      listeners.clear()
    },
  }
}
