type Options = {
  createSource?: (url: string) => EventSource
  retryDelayMs?: number
  idleTimeoutMs?: number
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
  let source: EventSource
  let stopped = false
  let timer: ReturnType<typeof setTimeout> | undefined
  let idleTimer: ReturnType<typeof setTimeout> | undefined
  const reconnect = () => {
    if (stopped) return
    if (timer !== undefined) clearTimeout(timer)
    timer = undefined
    source.close()
    connect()
  }
  const activity = () => {
    if (idleTimer !== undefined) clearTimeout(idleTimer)
    if (!stopped)
      idleTimer = setTimeout(reconnect, options.idleTimeoutMs ?? 45000)
  }
  const attach = (type: string, handler: EventListener) => {
    source.addEventListener(type, (event) => {
      activity()
      handler(event)
    })
  }
  const connect = () => {
    source = createSource(url)
    activity()
    source.addEventListener('open', activity)
    source.addEventListener('heartbeat', activity)
    for (const [type, handlers] of listeners) {
      for (const handler of handlers) attach(type, handler)
    }
    source.addEventListener('error', () => {
      // 2 is EventSource.CLOSED. CONNECTING retains native Last-Event-ID retry.
      if (stopped || source.readyState !== 2 || timer !== undefined) return
      timer = setTimeout(reconnect, options.retryDelayMs ?? 3000)
    })
  }
  connect()
  return {
    addEventListener(type: string, handler: EventListener) {
      const handlers = listeners.get(type) ?? new Set<EventListener>()
      handlers.add(handler)
      listeners.set(type, handlers)
      attach(type, handler)
    },
    close() {
      stopped = true
      if (timer !== undefined) clearTimeout(timer)
      if (idleTimer !== undefined) clearTimeout(idleTimer)
      source.close()
    },
  }
}
