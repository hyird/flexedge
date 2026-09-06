import { useEffect } from 'react'
import { useQueryClient } from '@tanstack/react-query'
import { toast } from 'sonner'
import type { ApiEnvelope } from '@/lib/api'
import {
  queryKeysForSyncEvents,
  syncEventCompletionMessage,
} from '@/lib/sync-events'
import type { SyncEventPage } from '@/lib/types'

export function SyncEventMonitor() {
  const queryClient = useQueryClient()

  useEffect(() => {
    const stream = new EventSource('/api/sync-events/stream', {
      withCredentials: true,
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
