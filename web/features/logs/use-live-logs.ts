import { useCallback, useMemo, useSyncExternalStore } from 'react'
import { useQueryClient } from '@tanstack/react-query'
import { registerSessionCleanup } from '@/features/auth/session-lifecycle'
import { recoverStreamSession } from '@/features/auth/session-expiry'
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
      void recoverStreamSession(client).then((valid) => { if (valid) created.reconnect() })
    })
    return created
  }, [client, url, parseLogs])
  const snapshot = useSyncExternalStore(
    useCallback(
      (listener: () => void) => {
        if (!enabled) return pausedSubscription()
        return registerSessionCleanup(client, store.subscribe(listener))
      },
      [client, enabled, store]
    ),
    store.getSnapshot
  )
  return {
    logs: snapshot.logs,
    connected: enabled && snapshot.connected,
    error: enabled ? snapshot.error : null,
  }
}
