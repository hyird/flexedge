import { useCallback, useMemo, useSyncExternalStore } from 'react'
import { useQueryClient } from '@tanstack/react-query'
import { useTabPanelLoading } from '@/components/tab-panel-hold'
import { recoverStreamSession } from '@/features/auth/session-expiry'
import { registerSessionCleanup } from '@/features/auth/session-lifecycle'
import { createLiveLogStore, type LogEntry } from './live-log-store'

const pausedSubscription = () => () => undefined

export function useLiveLogs<T extends LogEntry>(
  url: string,
  parseLogs: (raw: string) => T[],
  enabled = true
) {
  const client = useQueryClient()
  const store = useMemo(() => {
    const created = createLiveLogStore(url, parseLogs, undefined, () => {
      void recoverStreamSession(client).then((valid) => {
        if (valid) created.reconnect()
      })
    })
    return created
  }, [client, url, parseLogs])
  const snapshot = useSyncExternalStore(
    useCallback(
      (listener: () => void) => {
        if (!enabled) return pausedSubscription()
        // Subscribe after the commit's effects: a detail layout that disables
        // its live query in the same commit marks it departed only in its own
        // (later) effect, and the store releases departed queries before it
        // opens the log stream.
        let release: (() => void) | undefined
        let cancelled = false
        queueMicrotask(() => {
          if (!cancelled)
            release = registerSessionCleanup(client, store.subscribe(listener))
        })
        return () => {
          cancelled = true
          release?.()
        }
      },
      [client, enabled, store]
    ),
    store.getSnapshot
  )
  const loading = enabled && snapshot.loading
  // A switch to this tab keeps the panel's height until the first batch.
  useTabPanelLoading(loading)
  return {
    logs: snapshot.logs,
    connected: enabled && snapshot.connected,
    /** Opening: the first batch has not arrived yet (nor failed). */
    loading,
    error: enabled ? snapshot.error : null,
    reconnect: store.reconnect,
  }
}
