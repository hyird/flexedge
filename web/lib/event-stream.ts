type Options = {
  createSource?: (url: string) => EventSource
  retryDelayMs?: number
  // null for endpoints whose heartbeat frames are not dispatched as DOM events.
  idleTimeoutMs?: number | null
  // Cursor streams replace connections themselves so a fixed query cursor never
  // overrides a newer Last-Event-ID header during a native retry.
  resumeQueryParam?: string
}

/** Native EventSource retries interrupted streams, but terminal HTTP errors
 * (for example a proxy's 500 during deployment) can leave it CLOSED forever.
 * A proxy can also leave a dead connection open. Heartbeats bound that silence;
 * reconnecting ready reconciles current data without HTTP polling.
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
    if (stopped) return
    if (timer !== undefined) clearTimeout(timer)
    timer = undefined
    retireSource()
    connect()
  }
  const activity = () => {
    if (idleTimer !== undefined) clearTimeout(idleTimer)
    if (!stopped && options.idleTimeoutMs !== null)
      idleTimer = setTimeout(reconnect, options.idleTimeoutMs ?? 45000)
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
        stopped = true
        if (timer !== undefined) clearTimeout(timer)
        if (idleTimer !== undefined) clearTimeout(idleTimer)
        retireSource()
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
    attach(connection, 'open', () => undefined)
    attach(connection, 'heartbeat', () => undefined)
    for (const [type, handlers] of listeners) {
      for (const handler of handlers) attach(connection, type, handler)
    }
    const onError = () => {
      // Snapshot streams retain native retries; cursor streams use their latest
      // delivered id for every replacement, including transport interruption.
      if (
        stopped ||
        source !== connection ||
        (!options.resumeQueryParam && connection.readyState !== 2) ||
        timer !== undefined
      )
        return
      if (options.resumeQueryParam) connection.close()
      timer = setTimeout(reconnect, options.retryDelayMs ?? 3000)
    }
    connection.addEventListener('error', onError)
    detachListeners.push(() => connection.removeEventListener('error', onError))
  }
  connect()
  return {
    addEventListener(type: string, handler: EventListener) {
      if (stopped || !source) return
      const handlers = listeners.get(type) ?? new Set<EventListener>()
      if (handlers.has(handler)) return
      handlers.add(handler)
      listeners.set(type, handlers)
      attach(source, type, handler)
    },
    close() {
      if (stopped) return
      stopped = true
      if (timer !== undefined) clearTimeout(timer)
      if (idleTimer !== undefined) clearTimeout(idleTimer)
      retireSource()
      listeners.clear()
    },
  }
}
