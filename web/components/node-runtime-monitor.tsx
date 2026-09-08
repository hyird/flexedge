import { useEffect } from 'react'
import { useQueryClient } from '@tanstack/react-query'
import { reconnectingEventSource } from '@/lib/event-stream'
import { queryKeys } from '@/lib/query-keys'

/**
 * Keeps node-derived resource views current from the server's state-change
 * stream. This intentionally does not poll: reconnecting emits `ready`, so a
 * brief transport interruption cannot leave the visible table stale.
 */
export function NodeRuntimeMonitor() {
  const queryClient = useQueryClient()

  useEffect(() => {
    const stream = reconnectingEventSource('/api/nodes/stream')
    const refresh = () => {
      void Promise.all([
        queryClient.invalidateQueries({ queryKey: queryKeys.tasks }),
        queryClient.invalidateQueries({ queryKey: queryKeys.nodes }),
        queryClient.invalidateQueries({ queryKey: queryKeys.clusters }),
        queryClient.invalidateQueries({ queryKey: queryKeys.websites }),
        queryClient.invalidateQueries({ queryKey: queryKeys.overview }),
      ])
    }
    stream.addEventListener('ready', refresh)
    stream.addEventListener('node-state', refresh)
    return () => stream.close()
  }, [queryClient])

  return null
}
