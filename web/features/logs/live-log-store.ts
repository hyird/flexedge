import { reconnectingEventSource } from '@/lib/event-stream'
import { releaseDepartedLiveQueries } from '@/lib/live-query'

export type LogEntry = { id: string; occurred_at: string }
export const liveLogCapacity = 1000
type LogSource = ReturnType<typeof reconnectingEventSource>

export function mergeLiveLogs<T extends LogEntry>(
  current: readonly T[],
  incoming: readonly T[]
): T[] {
  return [
    ...new Map(
      [...current, ...incoming].map((entry) => [entry.id, entry])
    ).values(),
  ]
    .sort((left, right) => right.occurred_at.localeCompare(left.occurred_at))
    .slice(0, liveLogCapacity)
}

// One store belongs to one resource URL. Connections exist only while observed;
// React rendering and stream callbacks never share component-local state.
export function createLiveLogStore<T extends LogEntry>(
  url: string,
  parseLogs: (raw: string) => T[],
  createSource: (url: string) => LogSource = (path) =>
    reconnectingEventSource(path, {
      resumeQueryParam: 'after',
      idleTimeoutMs: null,
    }),
  onSessionExpired: () => undefined = () => undefined
) {
  let snapshot = {
    logs: [] as T[],
    connected: false,
    error: null as string | null,
    /** The tail has not delivered its first batch yet (nor failed). */
    loading: true,
  }
  const listeners = new Set<() => void>()
  let source: LogSource | undefined
  const update = (next: typeof snapshot) => {
    snapshot = next
    for (const listener of listeners) listener()
  }
  const connect = () => {
    // A tab switch from a live detail to its log tail: close the departed
    // detail stream first so the page never holds two streams at once.
    releaseDepartedLiveQueries()
    const connection = createSource(url)
    source = connection
    connection.addEventListener('ready', () => {
      if (source === connection) update({ ...snapshot, connected: true })
    })
    connection.addEventListener('logs', (event) => {
      if (source !== connection) return
      let incoming: T[]
      try {
        incoming = parseLogs((event as MessageEvent<string>).data)
      } catch {
        update({
          ...snapshot,
          error: '日志数据格式不正确，当前记录已保留。',
          loading: false,
        })
        return
      }
      update({
        ...snapshot,
        logs: mergeLiveLogs(snapshot.logs, incoming),
        error: null,
        loading: false,
      })
    })
    connection.addEventListener('error', () => {
      if (source === connection)
        update({ ...snapshot, connected: false, loading: false })
    })
    connection.addEventListener('session-expired', () => {
      if (source !== connection) return
      update({
        logs: [],
        connected: false,
        error: '登录状态已失效，正在恢复会话。',
        loading: false,
      })
      onSessionExpired()
    })
  }
  return {
    reconnect() {
      if (!listeners.size) return
      const retired = source
      source = undefined
      retired?.close()
      update({ ...snapshot, connected: false, error: null })
      connect()
    },
    getSnapshot: () => snapshot,
    subscribe(listener: () => void) {
      // Each subscription has independent ownership even for the same callback.
      const notify = () => listener()
      listeners.add(notify)
      if (listeners.size === 1) connect()
      return () => {
        if (!listeners.delete(notify) || listeners.size) return
        const retired = source
        source = undefined
        retired?.close()
        snapshot = { ...snapshot, connected: false }
      }
    },
  }
}
