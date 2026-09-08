import { useEffect } from 'react'
import { useQueryClient } from '@tanstack/react-query'
import { toast } from 'sonner'
import { apiErrorMessage, type ApiEnvelope } from '@/lib/api'
import { queryKeys } from '@/lib/query-keys'
import {
  refreshSyncEventQueries,
  syncEventFailureMessage,
} from '@/lib/sync-events'
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
      for (const result of events.list) {
        if (result.outcome === 'failed') {
          toast.error(syncEventFailureMessage(result), {
            id: `sync-failed-${result.resource_type}-${result.resource_id}`,
          })
        }
      }

      void (async () => {
        try {
          await refreshSyncEventQueries(queryClient, events.list)
        } catch (error) {
          toast.error(`收到同步结果，但数据刷新失败：${apiErrorMessage(error)}`)
        }
      })()
    })
    return () => stream.close()
  }, [queryClient])

  return null
}
