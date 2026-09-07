import { useEffect } from 'react'
import { useQueryClient } from '@tanstack/react-query'
import { toast } from 'sonner'
import type { ApiEnvelope } from '@/lib/api'
import {
  queryKeysForSyncEvents,
  syncEventCompletionMessage,
} from '@/lib/sync-events'
import { queryKeys } from '@/lib/query-keys'
import type { SyncEventPage } from '@/lib/types'

export function SyncEventMonitor() {
  const queryClient = useQueryClient()

  useEffect(() => {
    const stream = new EventSource('/api/sync-events/stream', {
      withCredentials: true,
    })
    const refreshAll = () =>
      Promise.all([
        queryClient.invalidateQueries({ queryKey: queryKeys.overview }),
        queryClient.invalidateQueries({ queryKey: queryKeys.providers }),
        queryClient.invalidateQueries({ queryKey: queryKeys.dnsZones }),
        queryClient.invalidateQueries({ queryKey: queryKeys.certificates }),
        queryClient.invalidateQueries({ queryKey: queryKeys.clusters }),
        queryClient.invalidateQueries({ queryKey: queryKeys.nodes }),
        queryClient.invalidateQueries({ queryKey: queryKeys.websites }),
      ])

    // EventSource reconnects by itself after a transport interruption. The
    // server emits `ready` for every connection, making this the one
    // event-driven cache reconciliation point instead of an HTTP timer.
    stream.addEventListener('ready', () => {
      void refreshAll()
    })
    stream.addEventListener('sync-events', (event) => {
      const payload = JSON.parse(
        (event as MessageEvent<string>).data
      ) as ApiEnvelope<SyncEventPage>
      const events = payload.data
      if (!events.list.length) return

      void (async () => {
        await Promise.all(
          queryKeysForSyncEvents(events.list).map((queryKey) =>
            queryClient.invalidateQueries({ queryKey })
          )
        )
        for (const event of events.list) {
          if (event.outcome === 'completed') {
            toast.success(syncEventCompletionMessage(event))
          }
        }
      })()
    })
    return () => stream.close()
  }, [queryClient])

  return null
}
