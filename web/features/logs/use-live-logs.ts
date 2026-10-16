import { useQuery } from '@tanstack/react-query'
import { useTabPanelLoading } from '@/components/tab-panel-hold'
import type { LiveLogsQuery } from './data'
import type { LogEntry } from './log-event'

/** View state of a live log tail (`liveLogsQuery`), open while `enabled`. */
export function useLiveLogs<T extends LogEntry>(
  query: LiveLogsQuery<T>,
  enabled = true
) {
  const result = useQuery({ ...query, enabled })
  const loading = enabled && result.isPending
  // A switch to this tab keeps the panel's height until the first batch.
  useTabPanelLoading(loading)
  return {
    logs: result.data ?? [],
    connected: enabled && result.status === 'success',
    /** Opening: the first batch has not arrived yet (nor failed). */
    loading,
    error: enabled && result.error ? result.error.message : null,
    reconnect: () => void result.refetch(),
  }
}
